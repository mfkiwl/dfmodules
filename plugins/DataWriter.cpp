/**
 * @file DataWriter.cpp DataWriter class implementation
 *
 * This is part of the DUNE DAQ Software Suite, copyright 2020.
 * Licensing/copyright details are in the COPYING file that you should have
 * received with this code.
 */

#include "DataWriter.hpp"
#include "dfmodules/CommonIssues.hpp"
#include "dfmodules/datawriter/Nljs.hpp"
#include "dfmodules/datawriterinfo/InfoNljs.hpp"

#include "appfwk/DAQModuleHelper.hpp"
#include "daqdataformats/Fragment.hpp"
#include "dfmessages/TriggerDecision.hpp"
#include "dfmessages/TriggerRecord_serialization.hpp"
#include "logging/Logging.hpp"
#include "iomanager/IOManager.hpp"
#include "rcif/cmd/Nljs.hpp"

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

/**
 * @brief Name used by TRACE TLOG calls from this source file
 */
//#define TRACE_NAME "DataWriter"                   // NOLINT This is the default
enum
{
  TLVL_ENTER_EXIT_METHODS = 5,
  TLVL_CONFIG = 7,
  TLVL_WORK_STEPS = 10,
  TLVL_SEQNO_MAP_CONTENTS = 13,
  TLVL_FRAGMENT_HEADER_DUMP = 17
};

namespace dunedaq {
namespace dfmodules {

DataWriter::DataWriter(const std::string& name)
  : dunedaq::appfwk::DAQModule(name)
  , m_queue_timeout(100)
  , m_data_storage_is_enabled(true)
  , m_thread(std::bind(&DataWriter::do_work, this, std::placeholders::_1))
{
  register_command("conf", &DataWriter::do_conf);
  register_command("start", &DataWriter::do_start);
  register_command("stop", &DataWriter::do_stop);
  register_command("scrap", &DataWriter::do_scrap);
}

void
DataWriter::init(const data_t& init_data)
{
  TLOG_DEBUG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": Entering init() method";
  auto iom = iomanager::IOManager::get();
  auto qi = appfwk::connection_index(init_data, { "trigger_record_input", "token_output" });
  m_trigger_record_connection = qi["trigger_record_input"] ;
  // try to create the receiver to see test the connection anyway
  m_tr_receiver = iom -> get_receiver<std::unique_ptr<daqdataformats::TriggerRecord>>(m_trigger_record_connection);

  m_token_output = iom-> get_sender<dfmessages::TriggerDecisionToken>(qi["token_output"]);
  
  TLOG_DEBUG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": Exiting init() method";
}

void
DataWriter::get_info(opmonlib::InfoCollector& ci, int /*level*/)
{
  datawriterinfo::Info dwi;

  dwi.records_received = m_records_received_tot.load();
  dwi.new_records_received = m_records_received.exchange(0);
  dwi.records_written = m_records_written_tot.load();
  dwi.new_records_written = m_records_written.exchange(0);
  dwi.bytes_output = m_bytes_output_tot.load();
  dwi.new_bytes_output = m_bytes_output.exchange(0);
  dwi.writing_time = m_writing_ms.exchange(0);

  ci.add(dwi);
}
void
DataWriter::do_conf(const data_t& payload)
{
  TLOG_DEBUG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": Entering do_conf() method";

  datawriter::ConfParams conf_params = payload.get<datawriter::ConfParams>();
  m_data_storage_prescale = conf_params.data_storage_prescale;
  TLOG_DEBUG(TLVL_CONFIG) << get_name() << ": data_storage_prescale is " << m_data_storage_prescale;
  TLOG_DEBUG(TLVL_CONFIG) << get_name() << ": data_store_parameters are " << conf_params.data_store_parameters;
  m_min_write_retry_time_usec = conf_params.min_write_retry_time_usec;
  if (m_min_write_retry_time_usec < 1) {
    m_min_write_retry_time_usec = 1;
  }
  m_max_write_retry_time_usec = conf_params.max_write_retry_time_usec;
  m_write_retry_time_increase_factor = conf_params.write_retry_time_increase_factor;
  m_trigger_decision_connection = conf_params.decision_connection;

  // create the DataStore instance here
  try {
    m_data_writer = make_data_store(payload["data_store_parameters"]);
  } catch (const ers::Issue& excpt) {
    throw UnableToConfigure(ERS_HERE, get_name(), excpt);
  }

  // ensure that we have a valid dataWriter instance
  if (m_data_writer.get() == nullptr) {
    throw InvalidDataWriter(ERS_HERE, get_name());
  }

  TLOG_DEBUG(TLVL_WORK_STEPS) << get_name() << ": Sending initial TriggerDecisionToken to DFO to announce my presence";
  dfmessages::TriggerDecisionToken token;
  token.run_number = 0;
  token.trigger_number = 0;
  token.decision_destination = m_trigger_decision_connection;

  int wasSentSuccessfully = 5;
  do {
    try {
      m_token_output->send(std::move(token), m_queue_timeout);
      wasSentSuccessfully = 0;
    } catch (const ers::Issue& excpt) {
      std::ostringstream oss_warn;
      oss_warn << "Send with sender \"" << m_token_output->get_name() << "\" failed";
      ers::warning(iomanager::OperationFailed(ERS_HERE, oss_warn.str(), excpt));
      wasSentSuccessfully--;
    }
  } while (wasSentSuccessfully);
  TLOG_DEBUG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": Exiting do_conf() method";
}

void
DataWriter::do_start(const data_t& payload)
{
  TLOG_DEBUG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": Entering do_start() method";
  
  rcif::cmd::StartParams start_params = payload.get<rcif::cmd::StartParams>();
  m_data_storage_is_enabled = (!start_params.disable_data_storage);
  m_run_number = start_params.run;

 
  // 04-Feb-2021, KAB: added this call to allow DataStore to prepare for the run.
  // I've put this call fairly early in this method because it could throw an
  // exception and abort the run start.  And, it seems sensible to avoid starting
  // threads, etc. if we throw an exception.
  if (m_data_storage_is_enabled) {

    // ensure that we have a valid dataWriter instance
    if (m_data_writer.get() == nullptr) {
      // this check is done essentially to notify the user
      // in case the "start" has been called before the "conf"
      ers::fatal(InvalidDataWriter(ERS_HERE, get_name()));
    }
    
    try {
      m_data_writer->prepare_for_run(m_run_number);
    } catch (const ers::Issue& excpt) {
      throw UnableToStart(ERS_HERE, get_name(), m_run_number, excpt);
    }
  }

  m_seqno_counts.clear();
  
  m_records_received = 0;
  m_records_received_tot = 0;
  m_records_written = 0;
  m_records_written_tot = 0;
  m_bytes_output = 0;
  m_bytes_output_tot = 0;

  m_running.store(true);

  m_thread.start_working_thread(get_name());
  //iomanager::IOManager::get()->add_callback<std::unique_ptr<daqdataformats::TriggerRecord>>( m_trigger_record_connection,
  //											     bind( &DataWriter::receive_trigger_record, this, std::placeholders::_1) );

  TLOG() << get_name() << " successfully started for run number " << m_run_number;
  TLOG_DEBUG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": Exiting do_start() method";
}

void
DataWriter::do_stop(const data_t& /*args*/)
{
  TLOG_DEBUG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": Entering do_stop() method";

  m_running.store(false);
  m_thread.stop_working_thread(); 
  //iomanager::IOManager::get()->remove_callback<std::unique_ptr<daqdataformats::TriggerRecord>>( m_trigger_record_connection );

  // 04-Feb-2021, KAB: added this call to allow DataStore to finish up with this run.
  // I've put this call fairly late in this method so that any draining of queues
  // (or whatever) can take place before we finalize things in the DataStore.
  if (m_data_storage_is_enabled) {
    try {
      m_data_writer->finish_with_run(m_run_number);
    } catch (const std::exception& excpt) {
      ers::error(ProblemDuringStop(ERS_HERE, get_name(), m_run_number, excpt));
    }
  }

  TLOG() << get_name() << " successfully stopped for run number " << m_run_number;
  TLOG_DEBUG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": Exiting do_stop() method";
}

void
DataWriter::do_scrap(const data_t& /*payload*/)
{
  TLOG_DEBUG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": Entering do_scrap() method";

  // clear/reset the DataStore instance here
  m_data_writer.reset();

  TLOG_DEBUG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": Exiting do_scrap() method";
}

void
DataWriter::receive_trigger_record(std::unique_ptr<daqdataformats::TriggerRecord> & trigger_record_ptr)
{
  TLOG_DEBUG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": receiving a new TR ptr";

  ++m_records_received;
  ++m_records_received_tot;
  TLOG_DEBUG(TLVL_WORK_STEPS) << get_name() << ": Obtained the TriggerRecord for trigger number "
			      << trigger_record_ptr->get_header_ref().get_trigger_number() << "."
			      << trigger_record_ptr->get_header_ref().get_sequence_number()
			      << ", run number " << trigger_record_ptr->get_header_ref().get_run_number()
			      << " off the input connection";

  if (trigger_record_ptr->get_header_ref().get_run_number() != m_run_number) {
    ers::error(InvalidRunNumber(ERS_HERE, get_name(), "TriggerRecord", trigger_record_ptr->get_header_ref().get_run_number(),
                                m_run_number, trigger_record_ptr->get_header_ref().get_trigger_number(),
                                trigger_record_ptr->get_header_ref().get_sequence_number()));
    return;
  }

  // 03-Feb-2021, KAB: adding support for a data-storage prescale.
  // In this "if" statement, I deliberately compare the result of (N mod prescale) to 1
  // instead of zero, since I think that it would be nice to always get the first event
  // written out.
  if (m_data_storage_prescale <= 1 || ((m_records_received_tot.load() % m_data_storage_prescale) == 1)) {
    
    if (m_data_storage_is_enabled) {

      std::chrono::steady_clock::time_point start_time = std::chrono::steady_clock::now();
      
      bool should_retry = true;
      size_t retry_wait_usec = m_min_write_retry_time_usec;
      do {
	should_retry = false;
	try {
	  m_data_writer->write(*trigger_record_ptr);
	  ++m_records_written;
	  ++m_records_written_tot;
	  m_bytes_output += trigger_record_ptr->get_total_size_bytes();
	  m_bytes_output_tot += trigger_record_ptr->get_total_size_bytes();
	} catch (const RetryableDataStoreProblem& excpt) {
	  should_retry = true;
	  ers::error(DataWritingProblem(ERS_HERE,
					get_name(),
					trigger_record_ptr->get_header_ref().get_trigger_number(),
					trigger_record_ptr->get_header_ref().get_sequence_number(),
					trigger_record_ptr->get_header_ref().get_run_number(),
					excpt));
	  if (retry_wait_usec > m_max_write_retry_time_usec) {
	    retry_wait_usec = m_max_write_retry_time_usec;
	  }
	  usleep(retry_wait_usec);
	  retry_wait_usec *= m_write_retry_time_increase_factor;
	} catch (const std::exception& excpt) {
	  ers::error(DataWritingProblem(ERS_HERE,
					get_name(),
					trigger_record_ptr->get_header_ref().get_trigger_number(),
					trigger_record_ptr->get_header_ref().get_sequence_number(),
					trigger_record_ptr->get_header_ref().get_run_number(),
					excpt));
	}
      } while (should_retry && m_running.load());

      std::chrono::steady_clock::time_point end_time = std::chrono::steady_clock::now();
      std::chrono::milliseconds writing_time = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
      m_writing_ms += writing_time.count();
    } //  if m_data_storage_is_enabled
  }
  
  bool send_trigger_complete_message = m_running.load();
  if (trigger_record_ptr->get_header_ref().get_max_sequence_number() > 0) {
    daqdataformats::trigger_number_t trigno = trigger_record_ptr->get_header_ref().get_trigger_number();
    if (m_seqno_counts.count(trigno) > 0) {
      ++m_seqno_counts[trigno];
    } else {
      m_seqno_counts[trigno] = 1;
    }
    // in the following comparison GT (>) is used since the counts are one-based and the
    // max sequence number is zero-based.
    if (m_seqno_counts[trigno] > trigger_record_ptr->get_header_ref().get_max_sequence_number()) {
      m_seqno_counts.erase(trigno);
    } else {
      // Using const .count and .at to avoid reintroducing element to map
      TLOG_DEBUG(TLVL_SEQNO_MAP_CONTENTS) << get_name() << ": the sequence number count for trigger number " << trigno
					  << " is " << (m_seqno_counts.count(trigno) ? m_seqno_counts.at(trigno) : 0) << " (number of entries "
					  << "in the seqno map is " << m_seqno_counts.size() << ").";
      send_trigger_complete_message = false;
    }
  }
  if (send_trigger_complete_message) {
    TLOG_DEBUG(TLVL_WORK_STEPS) << get_name() << ": Pushing the TriggerDecisionToken for trigger number "
				<< trigger_record_ptr->get_header_ref().get_trigger_number()
				<< " onto the relevant output queue";
    dfmessages::TriggerDecisionToken token;
    token.run_number = m_run_number;
    token.trigger_number = trigger_record_ptr->get_header_ref().get_trigger_number();
    token.decision_destination = m_trigger_decision_connection;

    bool wasSentSuccessfully = false;
    do { 
      try {
	m_token_output -> send( std::move(token), m_queue_timeout );
	wasSentSuccessfully = true;
      } catch (const ers::Issue& excpt) {
	std::ostringstream oss_warn;
	oss_warn << "Send with sender \"" << m_token_output -> get_name() << "\" failed";
	ers::warning(iomanager::OperationFailed(ERS_HERE, oss_warn.str(), excpt));
      }
    } while (!wasSentSuccessfully && m_running.load());

  }
  
  TLOG_DEBUG(TLVL_ENTER_EXIT_METHODS) << get_name() << ": operations completed for TR";
} // NOLINT(readability/fn_size)

void
DataWriter::do_work(std::atomic<bool>& running_flag) {
  while (running_flag.load()) {
	  try {
		std::unique_ptr<daqdataformats::TriggerRecord> tr = m_tr_receiver-> receive(std::chrono::milliseconds(10));   
                receive_trigger_record(tr);
	  }
	  catch(const iomanager::TimeoutExpired& excpt) {
	  }
	  catch(const ers::Issue & excpt) {
		ers::warning(excpt);
	  }
  }
}

} // namespace dfmodules
} // namespace dunedaq

DEFINE_DUNE_DAQ_MODULE(dunedaq::dfmodules::DataWriter)
