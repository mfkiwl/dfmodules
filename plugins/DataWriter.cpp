/**
 * @file DataWriter.cpp DataWriter class implementation
 *
 * This is part of the DUNE DAQ Software Suite, copyright 2020.
 * Licensing/copyright details are in the COPYING file that you should have
 * received with this code.
 */

#include "DataWriter.hpp"
#include "dfmodules/CommonIssues.hpp"
#include "dfmodules/KeyedDataBlock.hpp"
#include "dfmodules/StorageKey.hpp"
#include "dfmodules/datawriter/Nljs.hpp"

#include "appfwk/DAQModuleHelper.hpp"
#include "dataformats/Fragment.hpp"
#include "dfmessages/TriggerDecision.hpp"
#include "dfmessages/TriggerInhibit.hpp"

#include "TRACE/trace.h"
#include "ers/ers.h"

#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

/**
 * @brief Name used by TRACE TLOG calls from this source file
 */
#define TRACE_NAME "DataWriter"      // NOLINT
#define TLVL_ENTER_EXIT_METHODS 10   // NOLINT
#define TLVL_CONFIG 12               // NOLINT
#define TLVL_WORK_STEPS 15           // NOLINT
#define TLVL_FRAGMENT_HEADER_DUMP 27 // NOLINT

namespace dunedaq {
namespace dfmodules {

DataWriter::DataWriter(const std::string& name)
  : dunedaq::appfwk::DAQModule(name)
  , thread_(std::bind(&DataWriter::do_work, this, std::placeholders::_1))
  , queueTimeout_(100)
  , triggerRecordInputQueue_(nullptr)
{
  register_command("conf", &DataWriter::do_conf);
  register_command("start", &DataWriter::do_start);
  register_command("stop", &DataWriter::do_stop);
}

void
DataWriter::init(const data_t& init_data)
{
  TLOG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": Entering init() method";
  auto qi = appfwk::qindex(
    init_data, { "trigger_record_input_queue", "trigger_decision_for_inhibit", "trigger_inhibit_output_queue" });
  try {
    triggerRecordInputQueue_.reset(new trigrecsource_t(qi["trigger_record_input_queue"].inst));
  } catch (const ers::Issue& excpt) {
    throw InvalidQueueFatalError(ERS_HERE, get_name(), "trigger_record_input_queue", excpt);
  }

  using trigdecsource_t = dunedaq::appfwk::DAQSource<dfmessages::TriggerDecision>;
  std::unique_ptr<trigdecsource_t> trig_dec_queue_for_inh;
  try {
    trig_dec_queue_for_inh.reset(new trigdecsource_t(qi["trigger_decision_for_inhibit"].inst));
  } catch (const ers::Issue& excpt) {
    throw InvalidQueueFatalError(ERS_HERE, get_name(), "trigger_decision_for_inhibit", excpt);
  }
  using triginhsink_t = dunedaq::appfwk::DAQSink<dfmessages::TriggerInhibit>;
  std::unique_ptr<triginhsink_t> trig_inh_output_queue;
  try {
    trig_inh_output_queue.reset(new triginhsink_t(qi["trigger_inhibit_output_queue"].inst));
  } catch (const ers::Issue& excpt) {
    throw InvalidQueueFatalError(ERS_HERE, get_name(), "trigger_inhibit_output_queue", excpt);
  }
  trigger_inhibit_agent_.reset(
    new TriggerInhibitAgent(get_name(), std::move(trig_dec_queue_for_inh), std::move(trig_inh_output_queue)));

  TLOG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": Exiting init() method";
}

void
DataWriter::do_conf(const data_t& payload)
{
  TLOG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": Entering do_conf() method";

  datawriter::ConfParams conf_params = payload.get<datawriter::ConfParams>();
  trigger_inhibit_agent_->set_threshold_for_inhibit(conf_params.threshold_for_inhibit);
  TLOG(TLVL_CONFIG) << get_name() << ": threshold_for_inhibit is " << conf_params.threshold_for_inhibit;
  TLOG(TLVL_CONFIG) << get_name() << ": data_store_parameters are " << conf_params.data_store_parameters;

  // create the DataStore instance here
  data_writer_ = makeDataStore(payload["data_store_parameters"]);

  TLOG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": Exiting do_conf() method";
}

void
DataWriter::do_start(const data_t& /*args*/)
{
  TLOG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": Entering do_start() method";
  trigger_inhibit_agent_->start_checking();
  thread_.start_working_thread();
  ERS_LOG(get_name() << " successfully started");
  TLOG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": Exiting do_start() method";
}

void
DataWriter::do_stop(const data_t& /*args*/)
{
  TLOG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": Entering do_stop() method";
  trigger_inhibit_agent_->stop_checking();
  thread_.stop_working_thread();
  ERS_LOG(get_name() << " successfully stopped");
  TLOG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": Exiting do_stop() method";
}

void
DataWriter::do_work(std::atomic<bool>& running_flag)
{
  TLOG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": Entering do_work() method";
  int32_t received_count = 0;

  // ensure that we have a valid dataWriter instance
  if (data_writer_.get() == nullptr) {
    throw InvalidDataWriterError(ERS_HERE, get_name());
  }

  while (running_flag.load()) {
    std::unique_ptr<dataformats::TriggerRecord> trigRecPtr;

    // receive the next TriggerRecord
    try {
      triggerRecordInputQueue_->pop(trigRecPtr, queueTimeout_);
      ++received_count;
      TLOG(TLVL_WORK_STEPS) << get_name() << ": Popped the TriggerRecord for trigger number "
                            << trigRecPtr->get_trigger_number() << " off the input queue";
    } catch (const dunedaq::appfwk::QueueTimeoutExpired& excpt) {
      // it is perfectly reasonable that there might be no data in the queue
      // some fraction of the times that we check, so we just continue on and try again
      continue;
    }

    // if we received a TriggerRecord, print out some debug information, if requested
    const auto & frag_vec = trigRecPtr->get_fragments();
    for (const auto& frag_ptr : frag_vec) {
      TLOG(TLVL_FRAGMENT_HEADER_DUMP) << get_name() << ": Memory contents for the Fragment from link "
                                      << frag_ptr->get_link_ID().link_number;
      const uint32_t* mem_ptr = static_cast<const uint32_t*>(frag_ptr->get_storage_location());
      for (int idx = 0; idx < 4; ++idx) {
        std::ostringstream oss_hexdump;
        oss_hexdump << "32-bit offset " << std::setw(2) << (idx * 5) << ":" << std::hex;
        for (int jdx = 0; jdx < 5; ++jdx) {
          oss_hexdump << " 0x" << std::setw(8) << std::setfill('0') << *mem_ptr;
          ++mem_ptr;
        }
        oss_hexdump << std::dec;
        TLOG(TLVL_FRAGMENT_HEADER_DUMP) << get_name() << ": " << oss_hexdump.str();
      }

      // write each Fragment to the DataStore
      // //StorageKey fragment_skey(trigRecPtr->get_run_number(), trigRecPtr->get_trigger_number, "FELIX",
      StorageKey fragment_skey(frag_ptr->get_run_number(),
                               frag_ptr->get_trigger_number(),
                               "FELIX",
                               frag_ptr->get_link_ID().APA_number,
                               frag_ptr->get_link_ID().link_number);
      KeyedDataBlock data_block(fragment_skey);
      data_block.unowned_data_start = frag_ptr->get_storage_location();
      data_block.data_size = frag_ptr->get_size();

      //data_block.unowned_trigger_record_header = 
      //data_block.trh_size =
      data_writer_->write(data_block);
    }

    // progress updates
    if ((received_count % 3) == 0) {
      std::ostringstream oss_prog;
      oss_prog << ": Processing trigger number " << trigRecPtr->get_trigger_number() << ", this is one of "
               << received_count << " trigger records received so far.";
      ers::info(ProgressUpdate(ERS_HERE, get_name(), oss_prog.str()));
    }

    // tell the TriggerInhibitAgent the trigger_number of this TriggerRecord so that
    // it can check whether an Inhibit needs to be asserted or cleared.
    trigger_inhibit_agent_->set_latest_trigger_number(trigRecPtr->get_trigger_number());
  }

  std::ostringstream oss_summ;
  oss_summ << ": Exiting the do_work() method, received trigger record messages for " << received_count << " triggers.";
  ers::info(ProgressUpdate(ERS_HERE, get_name(), oss_summ.str()));
  TLOG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": Exiting do_work() method";
}

} // namespace dfmodules
} // namespace dunedaq

DEFINE_DUNE_DAQ_MODULE(dunedaq::dfmodules::DataWriter)
