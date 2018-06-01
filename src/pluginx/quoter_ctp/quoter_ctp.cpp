/*
* Copyright (c) 2018-2018 the QuoteX authors
* All rights reserved.
*
* The project sponsor and lead author is Xu Rendong.
* E-mail: xrd@ustc.edu, QQ: 277195007, WeChat: ustc_xrd
* You can get more information at https://xurendong.github.io
* For names of other contributors see the contributors file.
*
* Commercial use of this code in source and binary forms is
* governed by a LGPL v3 license. You may get a copy from the
* root directory. Or else you should get a specific written
* permission from the project author.
*
* Individual and educational use of this code in source and
* binary forms is governed by a 3-clause BSD license. You may
* get a copy from the root directory. Certainly welcome you
* to contribute code of all sorts.
*
* Be sure to retain the above copyright notice and conditions.
*/

#include <fstream>
#include <algorithm> // transform

#define OTL_ODBC // ʹ�� SQL Server

#include <common/assist.h>
#include <common/winver.h>
#include <common/common.h>
#include <syslog/syslog.h>
#include <syscfg/syscfg.h>
#include <sysrtm/sysrtm.h>
#include <sysdbi_s/sysdbi_s.h>
#include <plugins/plugins.h>

#include "define_ctp.h"
#include "quoter_ctp.h"
#include "quoter_ctp_.h"

// �� <boost/asio.hpp> ֮���������Ȼ�ᱨ��WinSock.h has already been included
#import "C:\Program Files (x86)\Common Files\System\ado\msado20.tlb" named_guids rename("EOF", "adoEOF") // SQL Server

#define FILE_LOG_ONLY 1

QuoterCTP the_app; // ����

// Ԥ�� QuoterCTP_P ����ʵ��

QuoterCTP_P::QuoterCTP_P()
	: NetServer_X()
	, m_location( "" )
	, m_info_file_path( "" )
	, m_cfg_file_path( "" )
	, m_service_running( false )
	, m_work_thread_number( CFG_WORK_THREAD_NUM )
	, m_plugin_running( false )
	, m_task_id( 0 )
	, m_user_api( nullptr )
	, m_user_spi( nullptr )
	, m_subscribe_ok( false )
	, m_log_cate( "<Quoter_CTP>" ) {
	m_syslog = basicx::SysLog_S::GetInstance();
	m_syscfg = basicx::SysCfg_S::GetInstance();
	m_sysrtm = basicx::SysRtm_S::GetInstance();
	m_sysdbi_s = basicx::SysDBI_S::GetInstance();
	m_plugins = basicx::Plugins::GetInstance();
}

QuoterCTP_P::~QuoterCTP_P() {
	if( m_output_buf_snapshot_future != nullptr ) {
		delete [] m_output_buf_snapshot_future;
		m_output_buf_snapshot_future = nullptr;
	}

	std::map<std::string, ConSubOne*>::iterator it_os_m;
	for( it_os_m = m_csm_snapshot_future.m_map_con_sub_one.begin(); it_os_m != m_csm_snapshot_future.m_map_con_sub_one.end(); it_os_m++ ) {
		if( it_os_m->second != nullptr ) {
			delete it_os_m->second;
		}
	}
	m_csm_snapshot_future.m_map_con_sub_one.clear();

	std::list<ConSubOne*>::iterator it_osc_l;
	for( it_osc_l = m_csm_snapshot_future.m_list_one_sub_con_del.begin(); it_osc_l != m_csm_snapshot_future.m_list_one_sub_con_del.end(); it_osc_l++ ) {
		if( (*it_osc_l) != nullptr ) {
			delete (*it_osc_l);
		}
	}
	m_csm_snapshot_future.m_list_one_sub_con_del.clear();
}

void QuoterCTP_P::SetGlobalPath()
{
	m_location = m_plugins->GetPluginLocationByName( PLUGIN_NAME );
	m_cfg_file_path = m_plugins->GetPluginCfgFilePathByName( PLUGIN_NAME );
	m_info_file_path = m_plugins->GetPluginInfoFilePathByName( PLUGIN_NAME );
}

bool QuoterCTP_P::ReadConfig( std::string file_path )
{
	std::string log_info;

	pugi::xml_document document;
	if( !document.load_file( file_path.c_str() ) ) {
		FormatLibrary::StandardLibrary::FormatTo( log_info, "�򿪲������������Ϣ�ļ�ʧ�ܣ�{0}", file_path );
		LogPrint( basicx::syslog_level::c_error, m_log_cate, log_info );
		return false;
	}

	pugi::xml_node xmlPluginNode = document.document_element();
	if( xmlPluginNode.empty() ) {
		log_info = "��ȡ�������������Ϣ ���ڵ� ʧ�ܣ�";
		LogPrint( basicx::syslog_level::c_error, m_log_cate, log_info );
		return false;
	}
	
	m_configs.m_address = xmlPluginNode.child_value( "Address" );
	m_configs.m_strbroker_id = xmlPluginNode.child_value( "BrokerID" );
	m_configs.m_username = xmlPluginNode.child_value( "Username" );
	m_configs.m_password = xmlPluginNode.child_value( "Password" );
	m_configs.m_sub_list_from = xmlPluginNode.child_value( "SubListFrom" );

	if( "USER" == m_configs.m_sub_list_from ) { // �ӱ��������ļ���ȡ���ĺ�Լ�б�
		for( pugi::xml_node xmlPluginChildNode = xmlPluginNode.first_child(); !xmlPluginChildNode.empty(); xmlPluginChildNode = xmlPluginChildNode.next_sibling() ) {
			if( "SubList" == std::string( xmlPluginChildNode.name() ) ) {
				std::string strContract = xmlPluginChildNode.attribute( "Contract" ).value();
				m_vec_contract.push_back( strContract );
				FormatLibrary::StandardLibrary::FormatTo( log_info, "��Լ���룺{0}", strContract );
				LogPrint( basicx::syslog_level::c_info, m_log_cate, log_info, FILE_LOG_ONLY );
			}
		}
	}
	else if( "JYDB" == m_configs.m_sub_list_from ) { // �Ӿ�Դ���ݿ��ȡ���ĺ�Լ�б�
		m_configs.m_name_odbc = xmlPluginNode.child_value( "NameODBC" );
		m_configs.m_db_address = xmlPluginNode.child_value( "DB_Addr" );
		m_configs.m_db_port = atoi( xmlPluginNode.child_value( "DB_Port" ) );
		m_configs.m_db_username = xmlPluginNode.child_value( "DB_Username" );
		m_configs.m_db_password = xmlPluginNode.child_value( "DB_Password" );
		m_configs.m_db_database = xmlPluginNode.child_value( "DB_DataBase" );
	}
	else if( "CTP" == m_configs.m_sub_list_from ) { // ������ƽ̨��ȡ���ĺ�Լ�б�
		m_configs.m_qc_address = xmlPluginNode.child_value( "QC_Address" );
		m_configs.m_qc_broker_id = xmlPluginNode.child_value( "QC_BrokerID" );
		m_configs.m_qc_username = xmlPluginNode.child_value( "QC_Username" );
		m_configs.m_qc_password = xmlPluginNode.child_value( "QC_Password" );
	}

	// �й�ʱ�����˻���Ϣ
	if( "0" == m_configs.m_username ) {
		//m_configs.m_username = "88870001"; // ����ڻ���
		//m_configs.m_password = "17260317"; // ����ڻ���
	}
	if( "0" == m_configs.m_qc_username ) {
		//m_configs.m_qc_username = "88870001"; // ����ڻ���
		//m_configs.m_qc_password = "17260317"; // ����ڻ���
	}

	m_configs.m_need_dump = atoi( xmlPluginNode.child_value( "NeedDump" ) );
	m_configs.m_dump_path = xmlPluginNode.child_value( "DumpPath" );
	m_configs.m_data_compress = atoi( xmlPluginNode.child_value( "DataCompress" ) );
	m_configs.m_data_encode = atoi( xmlPluginNode.child_value( "DataEncode" ) );
	m_configs.m_dump_time = atoi( xmlPluginNode.child_value( "DumpTime" ) );
	m_configs.m_init_time = atoi( xmlPluginNode.child_value( "InitTime" ) );
	m_configs.m_night_time = atoi( xmlPluginNode.child_value( "NightTime" ) );

	//FormatLibrary::StandardLibrary::FormatTo( log_info, "{0} {1} {2} {3} {4} {5} {6} {7}", m_configs.m_address, m_configs.m_strbroker_id, m_configs.m_username, m_configs.m_password, 
	//	m_configs.m_sub_list_from, m_configs.m_dump_time, m_configs.m_init_time, m_configs.m_night_time );
	//LogPrint( basicx::syslog_level::c_debug, m_log_cate, log_info );

	log_info = "�������������Ϣ��ȡ��ɡ�";
	LogPrint( basicx::syslog_level::c_info, m_log_cate, log_info );

	return true;
}

void QuoterCTP_P::LogPrint( basicx::syslog_level log_level, std::string& log_cate, std::string& log_info, int32_t log_show/* = 0*/ ) {
	if( 0 == log_show ) {
		m_syslog->LogPrint( log_level, log_cate, "LOG>: CTP " + log_info ); // ����̨
		m_sysrtm->LogTrans( log_level, log_cate, log_info ); // Զ�̼��
	}
	m_syslog->LogWrite( log_level, log_cate, log_info );
}

bool QuoterCTP_P::Initialize() {
	SetGlobalPath();
	ReadConfig( m_cfg_file_path );

	m_thread_service = boost::make_shared<boost::thread>( boost::bind( &QuoterCTP_P::CreateService, this ) );
	m_thread_user_request = boost::make_shared<boost::thread>( boost::bind( &QuoterCTP_P::HandleUserRequest, this ) );

	return true;
}

bool QuoterCTP_P::InitializeExt() {
	return true;
}

bool QuoterCTP_P::StartPlugin() {
	std::string log_info;
	try {
		std::string strInitInfo;

		log_info = "��ʼ���ò�� ....";
		LogPrint( basicx::syslog_level::c_info, m_log_cate, log_info );

		if( CheckSingleMutex() == false ) { // �������Ƽ��
		    return false;
		}

		StartNetServer();
		m_thread_on_timer = boost::make_shared<boost::thread>( boost::bind( &QuoterCTP_P::OnTimer, this ) );

		// TODO����Ӹ����ʼ������

		log_info = "���������ɡ�";
		LogPrint( basicx::syslog_level::c_info, m_log_cate, log_info );

		m_plugin_running = true; // ��Ҫ�ڴ����߳�ǰ��ֵΪ��

		return true;
	} // try
	catch( ... ) {
		log_info = "�������ʱ����δ֪����";
		LogPrint( basicx::syslog_level::c_error, m_log_cate, log_info );
	}

	return false;
}

bool QuoterCTP_P::IsPluginRun() {
	return m_plugin_running;
}

bool QuoterCTP_P::StopPlugin() {
	std::string log_info;
	try {
		std::string strInitInfo;

		log_info = "��ʼֹͣ��� ....";
		LogPrint( basicx::syslog_level::c_info, m_log_cate, log_info );

		// TODO����Ӹ��෴��ʼ������

		log_info = "���ֹͣ��ɡ�";
		LogPrint( basicx::syslog_level::c_info, m_log_cate, log_info );

		m_plugin_running = false;

		return true;
	} // try
	catch( ... ) {
		log_info = "���ֹͣʱ����δ֪����";
		LogPrint( basicx::syslog_level::c_error, m_log_cate, log_info );
	}

	return false;
}

bool QuoterCTP_P::UninitializeExt() {
	return true;
}

bool QuoterCTP_P::Uninitialize() {
	if( true == m_service_running ) {
		m_service_running = false;
		m_service->stop();
	}
	
	return true;
}

bool QuoterCTP_P::AssignTask( int32_t task_id, int32_t identity, int32_t code, std::string& data ) {
	try {
		m_task_list_lock.lock();
		bool bWriteInProgress = !m_list_task.empty();
		TaskItem TaskItemTemp;
		m_list_task.push_back( TaskItemTemp );
		TaskItem& TaskItemRef = m_list_task.back();
		TaskItemRef.m_task_id = task_id;
		TaskItemRef.m_identity = identity;
		TaskItemRef.m_code = code;
		TaskItemRef.m_data = data;
		m_task_list_lock.unlock();
		
		if( !bWriteInProgress && true == m_service_running ) {
			m_service->post( boost::bind( &QuoterCTP_P::HandleTaskMsg, this ) );
		}

		return true;
	}
	catch( std::exception& ex ) {
		std::string log_info;
		FormatLibrary::StandardLibrary::FormatTo( log_info, "��� TaskItem ��Ϣ �쳣��{0}", ex.what() );
		LogPrint( basicx::syslog_level::c_error, m_log_cate, log_info );
	}

	return false;
}

void QuoterCTP_P::CreateService()
{
	std::string log_info;

	log_info = "����������������߳����, ��ʼ��������������� ...";
	LogPrint( basicx::syslog_level::c_info, m_log_cate, log_info );

	try {
		try {
			m_service = boost::make_shared<boost::asio::io_service>();
			boost::asio::io_service::work Work( *m_service );

			for( int i = 0; i < m_work_thread_number; i++ ) {
				thread_ptr pThread( new boost::thread( boost::bind( &boost::asio::io_service::run, m_service ) ) );
				m_vec_work_thread.push_back( pThread );
			}

			m_service_running = true;

			for( int i = 0; i < (int)m_vec_work_thread.size(); i++ ) { // �ȴ������߳��˳�
				m_vec_work_thread[i]->join();
			}
		}
		catch( std::exception& ex ) {
			FormatLibrary::StandardLibrary::FormatTo( log_info, "����������� ��ʼ�� �쳣��{0}", ex.what() );
			LogPrint( basicx::syslog_level::c_error, m_log_cate, log_info );
		}
	} // try
	catch( ... ) {
		log_info = "������������̷߳���δ֪����";
		LogPrint( basicx::syslog_level::c_fatal, m_log_cate, log_info );
	}

	if( true == m_service_running ) {
		m_service_running = false;
		m_service->stop();
	}

	log_info = "������������߳��˳���";
	LogPrint( basicx::syslog_level::c_warn, m_log_cate, log_info );
}

void QuoterCTP_P::HandleTaskMsg() {
	std::string log_info;

	try {
		std::string strResultData;
		TaskItem* pTaskItem = &m_list_task.front(); // �϶���

		//FormatLibrary::StandardLibrary::FormatTo( log_info, "��ʼ���� {0} ������ ...", pTaskItem->m_task_id );
		//LogPrint( basicx::syslog_level::c_info, m_log_cate, log_info );

		try {
			Request RequestTemp;
			RequestTemp.m_task_id = pTaskItem->m_task_id;
			RequestTemp.m_identity = pTaskItem->m_identity;
			RequestTemp.m_code = pTaskItem->m_code;

			int function = 0;
			if( NW_MSG_CODE_JSON == pTaskItem->m_code ) {
				if( m_json_reader.parse( pTaskItem->m_data, RequestTemp.m_req_json, false ) ) { // �����ģ�std::string strUser = StringToGB2312( jsRootR["user"].asString() );
					function = RequestTemp.m_req_json["function"].asInt();
				}
				else {
					FormatLibrary::StandardLibrary::FormatTo( log_info, "�������� {0} ʱ���� JSON ����ʧ�ܣ�", pTaskItem->m_task_id );
					strResultData = OnErrorResult( function, -1, log_info, pTaskItem->m_code );
				}
			}
			else {
				FormatLibrary::StandardLibrary::FormatTo( log_info, "�������� {0} ʱ���ݱ����ʽ�쳣��", pTaskItem->m_task_id );
				pTaskItem->m_code = NW_MSG_CODE_JSON; // �����ʽδ֪ʱĬ��ʹ��
				strResultData = OnErrorResult( function, -1, log_info, pTaskItem->m_code );
			}

			if( function > 0 ) {
				switch( function ) {
				case TD_FUNC_QUOTE_ADDSUB: // ��������
					m_user_request_list_lock.lock();
					m_list_user_request.push_back( RequestTemp );
					m_user_request_list_lock.unlock();
					break;
				case TD_FUNC_QUOTE_DELSUB: // �˶�����
					m_user_request_list_lock.lock();
					m_list_user_request.push_back( RequestTemp );
					m_user_request_list_lock.unlock();
					break;
				default: // �Ự�Դ�������
					if( "" == strResultData ) { // ���� strResultData ����
						FormatLibrary::StandardLibrary::FormatTo( log_info, "�������� {0} ʱ���ܱ��δ֪��function��{1}", pTaskItem->m_task_id, function );
						strResultData = OnErrorResult( function, -1, log_info, RequestTemp.m_code );
					}
				}
			}
			else {
				if( "" == strResultData ) { // ���� strResultData ����
					FormatLibrary::StandardLibrary::FormatTo( log_info, "�������� {0} ʱ���ܱ���쳣��function��{1}", pTaskItem->m_task_id, function );
					strResultData = OnErrorResult( function, -1, log_info, RequestTemp.m_code );
				}
			}
		}
		catch( ... ) {
			FormatLibrary::StandardLibrary::FormatTo( log_info, "�������� {0} ʱ�������󣬿��ܱ����ʽ�쳣��", pTaskItem->m_task_id );
			pTaskItem->m_code = NW_MSG_CODE_JSON; // �����ʽδ֪ʱĬ��ʹ��
			strResultData = OnErrorResult( 0, -1, log_info, pTaskItem->m_code );
		}

		if( strResultData != "" ) { // ������ת��ǰ�ͷ���������
			CommitResult( pTaskItem->m_task_id, pTaskItem->m_identity, pTaskItem->m_code, strResultData );
		}

		FormatLibrary::StandardLibrary::FormatTo( log_info, "���� {0} ��������ɡ�", pTaskItem->m_task_id );
		//LogPrint( basicx::syslog_level::c_info, m_log_cate, log_info );

		m_task_list_lock.lock();
		m_list_task.pop_front();
		bool bWriteOnProgress = !m_list_task.empty();
		m_task_list_lock.unlock();

		if( bWriteOnProgress && true == m_service_running ) {
			m_service->post( boost::bind( &QuoterCTP_P::HandleTaskMsg, this ) );
		}
	}
	catch( std::exception& ex ) {
		FormatLibrary::StandardLibrary::FormatTo( log_info, "���� TaskItem ��Ϣ �쳣��{0}", ex.what() );
		LogPrint( basicx::syslog_level::c_error, m_log_cate, log_info );
	}
}

// �Զ��� QuoterCTP_P ����ʵ��

bool QuoterCTP_P::CheckSingleMutex() { // �������Ƽ��
	m_single_mutex = ::CreateMutex( NULL, FALSE, L"quoter_ctp" );
	if( GetLastError() == ERROR_ALREADY_EXISTS ) {
		std::string log_info = "��һ̨��������ֻ�������б����һ��ʵ����";
		LogPrint( basicx::syslog_level::c_error, m_log_cate, log_info );
		CloseHandle( m_single_mutex );
		return false;
	}
	return true;
}

void QuoterCTP_P::OnTimer() {
	std::string log_info;

	try {
		if( CreateDumpFolder() ) { // ���״ο���ʱ�ļ�����
			m_thread_init_api_spi = boost::make_shared<boost::thread>( boost::bind( &QuoterCTP_P::InitApiSpi, this ) );

			bool bForceDump = false; // ����˲���ε��� Dump ����
			bool bDoReSubscribe = false;
			bool bDoReSubscribeNight = false;
			bool bInitQuoteDataFile = false;
			while( true ) {
				for( int i = 0; i < m_configs.m_dump_time; i++ ) { // �����С��60��
					Sleep( 1000 );
					//if( ǰ��������������� )
					//{
					//	DumpSnapshotFuture();
					//	bForceDump = true;
					//}
				}
				if( false == bForceDump ) {
					DumpSnapshotFuture();
				}
				bForceDump = false;

				tm tmNowTime = basicx::GetNowTime();
				int nNowTime = tmNowTime.tm_hour * 100 + tmNowTime.tm_min;

				char cTimeTemp[32];
				strftime( cTimeTemp, 32, "%H%M%S", &tmNowTime);
				FormatLibrary::StandardLibrary::FormatTo( log_info, "{0} SnapshotFuture��R:{1}��W:{2}��C:{3}��S:{4}��", 
					cTimeTemp, m_cache_snapshot_future.m_recv_num.load(), m_cache_snapshot_future.m_dump_num.load(), m_cache_snapshot_future.m_comp_num.load(), m_cache_snapshot_future.m_send_num.load() );
				LogPrint( basicx::syslog_level::c_info, m_log_cate, log_info );

				m_cache_snapshot_future.m_dump_num = 0;
				m_cache_snapshot_future.m_comp_num = 0;
				m_cache_snapshot_future.m_send_num = 0;

				if( nNowTime == m_configs.m_init_time && false == bDoReSubscribe ) {
					DoReSubscribe();
					bDoReSubscribe = true;
				}

				if( nNowTime == m_configs.m_night_time && false == bDoReSubscribeNight ) {
					DoReSubscribe();
					bDoReSubscribeNight = true;
				}

				if( 600 == nNowTime && false == bInitQuoteDataFile ) {
					InitQuoteDataFile();
					bInitQuoteDataFile = true;

					ClearUnavailableConSub( m_csm_snapshot_future, true );
					log_info = "������Ч���ġ�";
					LogPrint( basicx::syslog_level::c_info, m_log_cate, log_info );
				}

				if( 500 == nNowTime ) {
					bDoReSubscribe = false;
					bDoReSubscribeNight = false;
					bInitQuoteDataFile = false;
				}
			}
		}
	} // try
	catch( ... ) {
		log_info = "�����ʱ�̷߳���δ֪����";
		LogPrint( basicx::syslog_level::c_fatal, m_log_cate, log_info );
	}

	log_info = "�����ʱ�߳��˳���";
	LogPrint( basicx::syslog_level::c_warn, m_log_cate, log_info );
}

void QuoterCTP_P::DoReSubscribe() {
	std::string log_info;

	log_info = "��ʼ�������³�ʼ�� ....";
	LogPrint( basicx::syslog_level::c_info, m_log_cate, log_info );

	// ���³�ʼ��ʱ�����˶����������صǳɹ�ʱ���¡�

	if( nullptr != m_user_api ) {
		// ������ʵ���ֶ��˳� InitApiSpi �߳�
		m_user_api->RegisterSpi( nullptr );
		m_user_api->Release();
		m_user_api = nullptr;
	}
	if( nullptr != m_user_spi ) {
		delete m_user_spi;
		m_user_spi = nullptr;
	}

	Sleep( 1000 ); // ����ȴ�ʱ��� InitApiSpi �˳���ʱ���

	m_thread_init_api_spi = boost::make_shared<boost::thread>( boost::bind( &QuoterCTP_P::InitApiSpi, this ) );

	log_info = "�������³�ʼ����ɡ�";
	LogPrint( basicx::syslog_level::c_info, m_log_cate, log_info );
}

bool QuoterCTP_P::CreateDumpFolder() {
	std::string log_info;

	std::string strDumpDataFolderPath = m_configs.m_dump_path;
	basicx::StringRightTrim( strDumpDataFolderPath, std::string( "\\" ) );

	int32_t number = MultiByteToWideChar( 0, 0, strDumpDataFolderPath.c_str(), -1, NULL, 0 );
	wchar_t* temp_dump_data_folder_path = new wchar_t[number];
	MultiByteToWideChar( 0, 0, strDumpDataFolderPath.c_str(), -1, temp_dump_data_folder_path, number );
	std::wstring w_dump_data_folder_path = temp_dump_data_folder_path;
	delete[] temp_dump_data_folder_path;
	WIN32_FIND_DATA FindDumpFolder;
	HANDLE hDumpFolder = FindFirstFile( w_dump_data_folder_path.c_str(), &FindDumpFolder );
	if( INVALID_HANDLE_VALUE == hDumpFolder ) {
		FindClose( hDumpFolder );
		FormatLibrary::StandardLibrary::FormatTo( log_info, "ʵʱ�������ݴ洢�ļ��в����ڣ�{0}", strDumpDataFolderPath );
		LogPrint( basicx::syslog_level::c_error, m_log_cate, log_info );
		return false;
	}
	FindClose( hDumpFolder );

	strDumpDataFolderPath += "\\Future";
	CreateDirectoryA( strDumpDataFolderPath.c_str(), NULL );

	m_cache_snapshot_future.m_folder_path = strDumpDataFolderPath + "\\Market";
	CreateDirectoryA( m_cache_snapshot_future.m_folder_path.c_str(), NULL );

	InitQuoteDataFile(); // �״�����ʱ

	return true;
}

void QuoterCTP_P::InitQuoteDataFile() {
	char cDateTemp[9];
	tm tmNowTime = basicx::GetNowTime();
	strftime( cDateTemp, 9, "%Y%m%d", &tmNowTime);
	FormatLibrary::StandardLibrary::FormatTo( m_cache_snapshot_future.m_file_path, "{0}\\{1}.hq", m_cache_snapshot_future.m_folder_path, cDateTemp );

	bool bIsNewFile = false;
	int32_t number = MultiByteToWideChar( 0, 0, m_cache_snapshot_future.m_file_path.c_str(), -1, NULL, 0 );
	wchar_t* temp_cache_file_path = new wchar_t[number];
	MultiByteToWideChar( 0, 0, m_cache_snapshot_future.m_file_path.c_str(), -1, temp_cache_file_path, number );
	std::wstring w_cache_file_path = temp_cache_file_path;
	delete[] temp_cache_file_path;
	WIN32_FIND_DATA FindFileTemp;
	HANDLE hFindFile = FindFirstFile( w_cache_file_path.c_str(), &FindFileTemp );
	if( INVALID_HANDLE_VALUE == hFindFile ) { // �����ļ�������
		bIsNewFile = true;
	}
	FindClose( hFindFile );

	if( true == bIsNewFile ) { // ������д�� 32 �ֽ��ļ�ͷ��
		std::ofstream File_SnapshotFuture;
		File_SnapshotFuture.open( m_cache_snapshot_future.m_file_path, std::ios::in | std::ios::out | std::ios::binary | std::ios::app );
		if( File_SnapshotFuture ) {
			char cHeadTemp[32] = { 0 };
			cHeadTemp[0] = SNAPSHOT_FUTURE_VERSION; // �ṹ��汾
			File_SnapshotFuture.write( cHeadTemp, 32 );
			File_SnapshotFuture.close();
		}
		else {
			std::string log_info;
			FormatLibrary::StandardLibrary::FormatTo( log_info, "SnapshotFuture������ת���ļ�ʧ�ܣ�{0}", m_cache_snapshot_future.m_file_path );
			LogPrint( basicx::syslog_level::c_error, m_log_cate, log_info );
		}
	}

	m_cache_snapshot_future.m_recv_num = 0;
	m_cache_snapshot_future.m_dump_num = 0;
	m_cache_snapshot_future.m_comp_num = 0;
	m_cache_snapshot_future.m_send_num = 0;
	m_cache_snapshot_future.m_local_index = 0;
}

void QuoterCTP_P::InitApiSpi() {
	std::string log_info;

	m_subscribe_count = 0;

	try {
		log_info = "��ʼ��ʼ���ӿ� ....";
		LogPrint( basicx::syslog_level::c_info, m_log_cate, log_info );
		std::string flow_path = m_syscfg->GetPath_ExtFolder() + "\\";
		m_user_api = CThostFtdcMdApi::CreateFtdcMdApi( flow_path.c_str() );
		m_user_spi = new CThostFtdcMdSpiImpl( m_user_api, this );
		m_user_api->RegisterSpi( m_user_spi );
		m_user_api->RegisterFront( const_cast<char*>(m_configs.m_address.c_str()) );
		m_user_api->Init();
		log_info = "��ʼ���ӿ���ɡ��ȴ�������Ӧ ....";
		LogPrint( basicx::syslog_level::c_info, m_log_cate, log_info );
	}
	catch( ... ) {
		log_info = "��ʼ���ӿ�ʱ����δ֪����";
		LogPrint( basicx::syslog_level::c_error, m_log_cate, log_info );
	}

	if( nullptr != m_user_api ) {
		m_user_api->Join();
	}

	log_info = "�ӿڳ�ʼ���߳�ֹͣ������";
	LogPrint( basicx::syslog_level::c_warn, m_log_cate, log_info );

	// Ŀǰ�����˶�����
	//if( true == m_subscribe_ok ) { // �˶�����
	//	m_subscribe_ok = false;

	//	int nContractNum = m_vec_contract.size();
	//	char** ppcContract = new char*[nContractNum];
	//	for( int i = 0; i < nContractNum; i++ ) {
	//	    ppcContract[i] = const_cast<char*>(m_vec_contract[i].c_str());
	//	}

	//	int nRet = m_user_api->UnSubscribeMarketData( ppcContract, nContractNum );
	//	if( 0 == nRet ) {
	//	    log_info = "�������� �˶� ����ɹ���";
	//	    LogPrint( basicx::syslog_level::c_info, m_log_cate, log_info );
	//	}
	//	else {
	//	    log_info = "�������� �˶� ����ʧ�ܣ�";
	//	    LogPrint( basicx::syslog_level::c_error, m_log_cate, log_info );
	//	}

	//	delete [] ppcContract;
	//}

	//Sleep( 2500 ); // �ȴ��˶�����

	// Ŀǰ������ DoReSubscribe ִ��
	//if( nullptr != m_user_api ) {
	//	m_user_api->RegisterSpi( nullptr );
	//	m_user_api->Release();
	//	m_user_api = nullptr;
	//}
	//if( nullptr != m_user_spi ) {
	//	delete m_user_spi;
	//	m_user_spi = nullptr;
	//}

	log_info = "�ӿڳ�ʼ���߳��˳���";
	LogPrint( basicx::syslog_level::c_warn, m_log_cate, log_info );
}

void QuoterCTP_P::GetSubListFromDB() {
	std::string log_info;

	m_vec_contract.clear(); // �����ѯʧ�ܣ���ᵼ�µ����޺�Լ����ɶ���

	std::string sql_query;
	FormatLibrary::StandardLibrary::FormatTo( sql_query, "select ContractCode, ExchangeCode from [{0}].[dbo].[Fut_ContractMain] A \
					 where ( A.ExchangeCode = 10 or A.ExchangeCode = 13 or A.ExchangeCode = 15 or A.ExchangeCode = 20 ) \
					 and A.EffectiveDate <= CONVERT( VARCHAR( 10 ), GETDATE(), 120 ) \
					 and A.LastTradingDate >= CONVERT( VARCHAR( 10 ), GETDATE(), 120 ) order by A.LastTradingDate", m_configs.m_db_database );

	basicx::WinVer WindowsVersion;
	LogPrint( basicx::syslog_level::c_info, m_log_cate, WindowsVersion.GetVersion() ); // ��ӡͬʱȡ�ð汾��Ϣ

	if( true == WindowsVersion.m_is_win_nt6 ) { // ʹ�� ADO ����
		log_info = "ʹ�� ADO ���� ��Դ ���ݿ⡣";
		LogPrint( basicx::syslog_level::c_info, m_log_cate, log_info );

		int32_t result = S_OK;
		ADODB::_Connection* connection = nullptr;
		ADODB::_Recordset* recordset = nullptr;
		
		result = m_sysdbi_s->Connect( connection, recordset, m_configs.m_db_address, m_configs.m_db_port, m_configs.m_db_username, m_configs.m_db_password, m_configs.m_db_database );
		if( FAILED( result ) ) {
			FormatLibrary::StandardLibrary::FormatTo( log_info, "�������ݿ�ʧ�ܣ�{0}", result );
			LogPrint( basicx::syslog_level::c_error, m_log_cate, log_info );
			return;
		}
		else {
			FormatLibrary::StandardLibrary::FormatTo( log_info, "�������ݿ�ɹ���{0}", result );
			LogPrint( basicx::syslog_level::c_info, m_log_cate, log_info );
		}

		if( true == m_sysdbi_s->Query( connection, recordset, sql_query ) ) {
			int64_t row_number = m_sysdbi_s->GetCount( recordset );
			if( row_number <= 0 ) {
				FormatLibrary::StandardLibrary::FormatTo( log_info, "��ѯ��ú�Լ�����¼�����쳣��{0}", row_number );
				LogPrint( basicx::syslog_level::c_error, m_log_cate, log_info );
			}
			else {
				while( !m_sysdbi_s->GetEOF( recordset ) ) {
					_variant_t v_contract_code;
					_variant_t v_exchange_code;
					v_contract_code = recordset->GetCollect( "ContractCode" );
					v_exchange_code = recordset->GetCollect( "ExchangeCode" );
					std::string contract_code = _bstr_t( v_contract_code );
					int32_t exchange_code = 0;
					if( v_exchange_code.vt != VT_NULL ) {
						exchange_code = (int32_t)v_exchange_code; // 10-�Ϻ��ڻ���������13-������Ʒ��������15-֣����Ʒ��������20-�й������ڻ�������
					}
					if( contract_code != "" ) {
						// ��ѯ���þ�Ϊ��д
						// �������ʹ�����Ʒ�ִ�����Сд��10��13
						// �н�����֣����Ʒ�ִ������д��15��20
						if( 10 == exchange_code || 13 == exchange_code ) {
							std::transform( contract_code.begin(), contract_code.end(), contract_code.begin(), tolower );
						}
						m_vec_contract.push_back( contract_code );
						FormatLibrary::StandardLibrary::FormatTo( log_info, "��Լ���룺{0} {1}", contract_code, exchange_code );
						LogPrint( basicx::syslog_level::c_info, m_log_cate, log_info, FILE_LOG_ONLY );
					}
					m_sysdbi_s->MoveNext( recordset );
				}

				FormatLibrary::StandardLibrary::FormatTo( log_info, "��ѯ��ú�Լ�����¼ {0} {1} ����", row_number, m_vec_contract.size() );
				LogPrint( basicx::syslog_level::c_info, m_log_cate, log_info );
			}
		}
		else {
			log_info = "��ѯ��Լ�����¼ʧ�ܣ�";
			LogPrint( basicx::syslog_level::c_error, m_log_cate, log_info );
		}

		m_sysdbi_s->Close( recordset );
		m_sysdbi_s->Release( connection, recordset );
	}
	else { // ʹ�� OTL ����
		log_info = "ʹ�� OTL ���� ��Դ ���ݿ⡣";
		LogPrint( basicx::syslog_level::c_info, m_log_cate, log_info );

		otl_connect otlConnect;
		otl_connect::otl_initialize(); // ��ʼ�� ODBC ����

		try {
			std::string strLogon;
			FormatLibrary::StandardLibrary::FormatTo( strLogon, "{0}/{1}@{2}", m_configs.m_db_username, m_configs.m_db_password, m_configs.m_name_odbc );
			otlConnect.rlogon( strLogon.c_str() ); // ���� ODBC

			otl_stream otlStream( 1, sql_query.c_str(), otlConnect );
			
			int nRecordNum = 0;
			while( !otlStream.eof() ) {
				char cContractCode[8];
				int exchange_code = 0;
				otlStream>>cContractCode>>exchange_code; // 10-�Ϻ��ڻ���������13-������Ʒ��������15-֣����Ʒ��������20-�й������ڻ�������
				std::string contract_code = cContractCode;
				if( contract_code != "" ) {
					// ��ѯ���þ�Ϊ��д
					// �������ʹ�����Ʒ�ִ�����Сд��10��13
					// �н�����֣����Ʒ�ִ������д��15��20
					if( 10 == exchange_code || 13 == exchange_code ) {
						std::transform( contract_code.begin(), contract_code.end(), contract_code.begin(), tolower );
					}
					m_vec_contract.push_back( contract_code );
					FormatLibrary::StandardLibrary::FormatTo( log_info, "��Լ���룺{0} {1}", contract_code, exchange_code );
					LogPrint( basicx::syslog_level::c_info, m_log_cate, log_info, FILE_LOG_ONLY );
				}
				nRecordNum++;
			}

			FormatLibrary::StandardLibrary::FormatTo( log_info, "��ѯ��ú�Լ�����¼ {0} {1} ����", nRecordNum, m_vec_contract.size() );
			LogPrint( basicx::syslog_level::c_info, m_log_cate, log_info );
		}
		catch( otl_exception& ex ) {
			FormatLibrary::StandardLibrary::FormatTo( log_info, "��ѯ��Լ�����¼�쳣��{0} {1} {2} {3}", 
				std::string( (char*)ex.msg, 1000 ), std::string( ex.stm_text, 2048 ), std::string( (char*)ex.sqlstate, 1000 ), std::string( ex.var_info, 256 ) );
			LogPrint( basicx::syslog_level::c_error, m_log_cate, log_info );
		}

		otlConnect.logoff(); // �Ͽ� ODBC ����
	}

	log_info = "�� ���ݿ� ��ȡ��Լ������ɡ�";
	LogPrint( basicx::syslog_level::c_info, m_log_cate, log_info );
}

void QuoterCTP_P::GetSubListFromCTP() { // ʵ�̲�����ʾ�� CTP ��ѯ�õ��ĺ�Լ�б���Ҳû������ָ���Լ
	std::string log_info;

	m_vec_contract.clear(); // �����ѯʧ�ܣ���ᵼ�µ����޺�Լ����ɶ���

	int nRet = 0;
	CThostFtdcTraderApi* user_api = nullptr;
	CThostFtdcTraderSpiImpl* pUserSpi = nullptr;
	try {
		log_info = "��ʼ��ʼ�� ��Լ��ѯ �ӿ� ....";
		LogPrint( basicx::syslog_level::c_info, m_log_cate, log_info );
		std::string flow_path = m_syscfg->GetPath_ExtFolder() + "\\";
		user_api = CThostFtdcTraderApi::CreateFtdcTraderApi( flow_path.c_str() );
		pUserSpi = new CThostFtdcTraderSpiImpl( user_api, this );
		user_api->RegisterSpi( pUserSpi );
		user_api->RegisterFront( const_cast<char*>(m_configs.m_qc_address.c_str()) );
		// ����Ҫ��������Ϣ
		pUserSpi->m_connect_ok = false;
		user_api->Init();
		log_info = "��ʼ�� ��Լ��ѯ �ӿ���ɡ��ȴ�������Ӧ ....";
		LogPrint( basicx::syslog_level::c_info, m_log_cate, log_info );

		int nTimeWait = 0;
		while( false == pUserSpi->m_connect_ok && nTimeWait < 5000 ) { // ���ȴ� 5 ��
			Sleep( 100 );
			nTimeWait += 100;
		}
		if( false == pUserSpi->m_connect_ok ) {
			log_info = "����ǰ�� ���ӳ�ʱ��";
			LogPrint( basicx::syslog_level::c_error, m_log_cate, log_info );
		}
		else {
			log_info = "����ǰ�� ������ɡ�";
			LogPrint( basicx::syslog_level::c_info, m_log_cate, log_info );

			// ��¼����
			nRet = pUserSpi->ReqUserLogin( m_configs.m_qc_broker_id, m_configs.m_qc_username, m_configs.m_qc_password );
			if( nRet != 0 ) { // -1��-2��-3
				FormatLibrary::StandardLibrary::FormatTo( log_info, "�û���¼ʱ ��¼����ʧ�ܣ�{0}", nRet );
				LogPrint( basicx::syslog_level::c_error, m_log_cate, log_info );
			}
			else {
				log_info = "�û���¼ �ύ�ɹ���";
				LogPrint( basicx::syslog_level::c_info, m_log_cate, log_info );

				int nTimeWait = 0;
				while( false == pUserSpi->m_login_ok && nTimeWait < 5000 ) { // ���ȴ� 5 ��
					if( true == pUserSpi->m_last_rsp_is_error ) {
						break;
					}
					Sleep( 100 );
					nTimeWait += 100;
				}
			}
			if( false == pUserSpi->m_login_ok ) {
				std::string strErrorMsg = pUserSpi->GetLastErrorMsg();
				FormatLibrary::StandardLibrary::FormatTo( log_info, "�û���¼ʱ ��¼ʧ�ܣ�{0}", strErrorMsg );
				LogPrint( basicx::syslog_level::c_error, m_log_cate, log_info );
			}
			else {
				log_info = "���׹�̨ ��¼��ɡ�";
				LogPrint( basicx::syslog_level::c_info, m_log_cate, log_info );

				// ��ѯ����
				nRet = pUserSpi->ReqQryInstrument();
				if( nRet != 0 ) { // -1��-2��-3
					FormatLibrary::StandardLibrary::FormatTo( log_info, "��Լ��ѯ �ύʧ�ܣ�{0}", nRet );
					LogPrint( basicx::syslog_level::c_error, m_log_cate, log_info );
				}
				else {
					log_info = "��Լ��ѯ �ύ�ɹ���";
					LogPrint( basicx::syslog_level::c_info, m_log_cate, log_info );

					int nTimeWait = 0;
					while( false == pUserSpi->m_qry_instrument && nTimeWait < 5000 ) { // ���ȴ� 5 ��
						Sleep( 100 );
						nTimeWait += 100;
					}
				}
				if( false == pUserSpi->m_qry_instrument ) {
					log_info = "��Լ��ѯ��ʱ��";
					LogPrint( basicx::syslog_level::c_error, m_log_cate, log_info );
				}
				else {
					FormatLibrary::StandardLibrary::FormatTo( log_info, "��Լ��ѯ�ɹ������� {0} ����", m_vec_contract.size() );
					LogPrint( basicx::syslog_level::c_info, m_log_cate, log_info );
				}

				// �ǳ����� // ���Բ�����ִ�� ReqUserLogout ��������ʾ�Ự�Ͽ���ǰ�������ж�(00001001)��������� Release ��Ȼ�ᷢ������Ϣ
				//nRet = pUserSpi->ReqUserLogout( m_configs.m_qc_broker_id, m_configs.m_qc_username );
				//if( nRet != 0 ) { //-1��-2��-3
				//	FormatLibrary::StandardLibrary::FormatTo( log_info, "�û��ǳ�ʱ �ǳ�����ʧ�ܣ�{0}", nRet );
				//	LogPrint( basicx::syslog_level::c_error, m_log_cate, log_info );
				//}
				//else {
				//	log_info = "�û��ǳ� �ύ�ɹ���";
				//	LogPrint( basicx::syslog_level::c_info, m_log_cate, log_info );

				//	int nTimeWait = 0;
				//	while( false == pUserSpi->m_logout_ok && nTimeWait < 5000 ) { // ���ȴ� 5 ��
				//		if( true == pUserSpi->m_last_rsp_is_error ) {
				//		    break;
				//	    }
				//		Sleep( 100 );
				//		nTimeWait += 100;
				//	}
				//}
				//if( false == pUserSpi->m_logout_ok ) {
				//	std::string strErrorMsg = pUserSpi->GetLastErrorMsg();
				//	FormatLibrary::StandardLibrary::FormatTo( log_info, "�û��ǳ�ʱ �ǳ�ʧ�ܣ�{0}", strErrorMsg );
				//	LogPrint( basicx::syslog_level::c_error, m_log_cate, log_info );
				//}
				//else {
				//	log_info = "���ڹ�̨ �ǳ���ɡ�";
				//	LogPrint( basicx::syslog_level::c_info, m_log_cate, log_info );
				//}
			}
		}

		//user_api->Join();
	}
	catch( ... ) {
		log_info = "������ƽ̨��ȡ��Լ����ʱ����δ֪����";
		LogPrint( basicx::syslog_level::c_error, m_log_cate, log_info );
	}

	if( user_api != nullptr ) {
		user_api->Release();
	} // ֻ�� Release �Ժ�Ż�ֹͣ����������Ϣ�����֮ǰ��ִ�� ReqUserLogout ���������ﻹ���½�һ�λỰ��֮���ٴ���ʾ�Ự�Ͽ���ǰ�������ж�(00000000)
	if( pUserSpi != nullptr ) {
		delete pUserSpi;
	}

	log_info = "�� ����ƽ̨ ��ȡ��Լ������ɡ�";
	LogPrint( basicx::syslog_level::c_info, m_log_cate, log_info );
}

void QuoterCTP_P::MS_AddData_SnapshotFuture( SnapshotFuture& snapshot_future_temp ) {
	//if( m_net_server_broad->Server_GetConnectCount() > 0 )
	if( m_csm_snapshot_future.m_map_con_sub_all.size() > 0 || m_csm_snapshot_future.m_map_con_sub_one.size() > 0 ) { // ȫ�г��͵�֤ȯ���Էֱ�ѹ���Լ���CPUѹ�����Ч�ʣ���һ�㻹��ȫ�г����ĵĶ�
		int nOutputBufLenSnapshotFuture = m_output_buf_len_snapshot_future; // ����ÿ�����¸�ֵ������ѹ������
		memset( m_output_buf_snapshot_future, 0, m_output_buf_len_snapshot_future );
		int nRet = gzCompress( (unsigned char*)&snapshot_future_temp, sizeof( snapshot_future_temp ), m_output_buf_snapshot_future, (uLongf*)&nOutputBufLenSnapshotFuture, m_data_compress );
		if( Z_OK == nRet ) { // ������ѹ��
			m_cache_snapshot_future.m_comp_num++;
			std::string strQuoteData( (char*)m_output_buf_snapshot_future, nOutputBufLenSnapshotFuture );
			strQuoteData = TD_FUNC_QUOTE_DATA_MARKET_FUTURE_NP_HEAD + strQuoteData;

			// �ȸ�ȫ�г��İɣ�ȫ�г������߿��ܻ�Ҫ�Լ����й���

			{ // �㲥��ȫ�г�������
				m_csm_snapshot_future.m_lock_con_sub_all.lock();
				std::map<int32_t, basicx::ConnectInfo*> mapConSub_All = m_csm_snapshot_future.m_map_con_sub_all;
				m_csm_snapshot_future.m_lock_con_sub_all.unlock();

				std::map<int32_t, basicx::ConnectInfo*>::iterator itCSA;
				for( itCSA = mapConSub_All.begin(); itCSA != mapConSub_All.end(); itCSA++ ) {
					if( true == m_net_server_broad->IsConnectAvailable( itCSA->second ) ) {
						m_net_server_broad->Server_SendData( itCSA->second, NW_MSG_TYPE_USER_DATA, m_data_encode, strQuoteData );
						m_cache_snapshot_future.m_send_num++;
					}
				}
			}

			{ // �㲥����֤ȯ������
				m_csm_snapshot_future.m_lock_con_sub_one.lock();
				std::map<std::string, ConSubOne*> mapConSub_One = m_csm_snapshot_future.m_map_con_sub_one;
				m_csm_snapshot_future.m_lock_con_sub_one.unlock();

				std::map<std::string, ConSubOne*>::iterator itCSO;
				itCSO = mapConSub_One.find( snapshot_future_temp.m_code );
				if( itCSO != mapConSub_One.end() ) {
					ConSubOne* pOneSubCon = itCSO->second; // �����ڼ� pOneSubCon ��ָ���󲻻ᱻɾ��

					pOneSubCon->m_lock_con_sub_one.lock();
					std::map<int32_t, basicx::ConnectInfo*> mapIdentity = pOneSubCon->m_map_identity;
					pOneSubCon->m_lock_con_sub_one.unlock();

					std::map<int32_t, basicx::ConnectInfo*>::iterator itID;
					for( itID = mapIdentity.begin(); itID != mapIdentity.end(); itID++ ) {
						if( true == m_net_server_broad->IsConnectAvailable( itID->second ) ) {
							m_net_server_broad->Server_SendData( itID->second, NW_MSG_TYPE_USER_DATA, m_data_encode, strQuoteData );
							m_cache_snapshot_future.m_send_num++;
						}
					}
				}
			}
		}
	}

	if( 1 == m_configs.m_need_dump ) {
		m_cache_snapshot_future.m_lock_cache.lock();
		m_cache_snapshot_future.m_vec_cache_put->push_back( snapshot_future_temp );
		m_cache_snapshot_future.m_lock_cache.unlock();
	}
}

void QuoterCTP_P::DumpSnapshotFuture() {
	std::string log_info;

	m_cache_snapshot_future.m_lock_cache.lock();
	if( m_cache_snapshot_future.m_vec_cache_put == &m_cache_snapshot_future.m_vec_cache_01 ) {
		m_cache_snapshot_future.m_vec_cache_put = &m_cache_snapshot_future.m_vec_cache_02;
		m_cache_snapshot_future.m_vec_cache_put->clear();
		m_cache_snapshot_future.m_vec_cache_out = &m_cache_snapshot_future.m_vec_cache_01;
	}
	else {
		m_cache_snapshot_future.m_vec_cache_put = &m_cache_snapshot_future.m_vec_cache_01;
		m_cache_snapshot_future.m_vec_cache_put->clear();
		m_cache_snapshot_future.m_vec_cache_out = &m_cache_snapshot_future.m_vec_cache_02;
	}
	m_cache_snapshot_future.m_lock_cache.unlock();

	int nOutDataNum = m_cache_snapshot_future.m_vec_cache_out->size();
	if( nOutDataNum > 0 ) { // ������������ʱ�����ļ�
		std::ofstream File_SnapshotFuture;
		File_SnapshotFuture.open( m_cache_snapshot_future.m_file_path, std::ios::in | std::ios::out | std::ios::binary | std::ios::app );
		if( File_SnapshotFuture ) {
			for( int i = 0; i < nOutDataNum; ++i ) {
				File_SnapshotFuture.write( (const char*)(&(*m_cache_snapshot_future.m_vec_cache_out)[i]), sizeof( SnapshotFuture ) );
			}
			File_SnapshotFuture.close(); // �Ѻ� flush() ����
			m_cache_snapshot_future.m_dump_num += nOutDataNum;
		}
		else {
			FormatLibrary::StandardLibrary::FormatTo( log_info, "SnapshotFuture����ת���ļ�ʧ�ܣ�{0}", m_cache_snapshot_future.m_file_path );
			LogPrint( basicx::syslog_level::c_error, m_log_cate, log_info );
		}
	}
}

void QuoterCTP_P::StartNetServer() {
	m_data_encode = m_configs.m_data_encode;
	m_data_compress = m_configs.m_data_compress;

	m_output_buf_len_snapshot_future = sizeof( SnapshotFuture ) + sizeof( SnapshotFuture ) / 3 + 128; // > ( nInputLen + 12 ) * 1.001;
	m_output_buf_snapshot_future = new unsigned char[m_output_buf_len_snapshot_future]; // ���㹻��

	basicx::CfgBasic* cfg_basic = m_syscfg->GetCfgBasic();

	m_net_server_broad = boost::make_shared<basicx::NetServer>();
	m_net_server_query = boost::make_shared<basicx::NetServer>();
	m_net_server_broad->ComponentInstance( this );
	m_net_server_query->ComponentInstance( this );
	basicx::NetServerCfg NetServerCfgTemp;
	NetServerCfgTemp.m_log_test = cfg_basic->m_debug_infos_server;
	NetServerCfgTemp.m_heart_check_time = cfg_basic->m_heart_check_server;
	NetServerCfgTemp.m_max_msg_cache_number = cfg_basic->m_max_msg_cache_server;
	NetServerCfgTemp.m_io_work_thread_number = cfg_basic->m_work_thread_server;
	NetServerCfgTemp.m_client_connect_timeout = cfg_basic->m_con_time_out_server;
	NetServerCfgTemp.m_max_connect_total_s = cfg_basic->m_con_max_server_server;
	NetServerCfgTemp.m_max_data_length_s = cfg_basic->m_data_length_server;
	m_net_server_broad->StartNetwork( NetServerCfgTemp );
	m_net_server_query->StartNetwork( NetServerCfgTemp );

	for( size_t i = 0; i < cfg_basic->m_vec_server_server.size(); i++ ) {
		if( "broad_ctp" == cfg_basic->m_vec_server_server[i].m_type && 1 == cfg_basic->m_vec_server_server[i].m_work ) {
			m_net_server_broad->Server_AddListen( "0.0.0.0", cfg_basic->m_vec_server_server[i].m_port, cfg_basic->m_vec_server_server[i].m_type ); // 0.0.0.0
		}
		if( "query_ctp" == cfg_basic->m_vec_server_server[i].m_type && 1 == cfg_basic->m_vec_server_server[i].m_work ) {
			m_net_server_query->Server_AddListen( "0.0.0.0", cfg_basic->m_vec_server_server[i].m_port, cfg_basic->m_vec_server_server[i].m_type ); // 0.0.0.0
		}
	}
}

void QuoterCTP_P::OnNetServerInfo( basicx::NetServerInfo& net_server_info_temp ) {
	// TODO������ȫ�г����ĺ͵�֤ȯ����
}

void QuoterCTP_P::OnNetServerData( basicx::NetServerData& net_server_data_temp ) {
	try {
		if( m_task_id >= 214748300/*0*/ ) { // 2 ^ 31 = 2147483648
			m_task_id = 0;
		}
		m_task_id++;

		if( "BroadCTP" == net_server_data_temp.m_node_type ) { // 1 // �ڵ����ͱ���������ļ�һ��
			AssignTask( m_task_id * 10 + 1, net_server_data_temp.m_identity, net_server_data_temp.m_code, net_server_data_temp.m_data ); // 1
		}
		else if( "QueryCTP" == net_server_data_temp.m_node_type ) { // 2 // �ڵ����ͱ���������ļ�һ��
			AssignTask( m_task_id * 10 + 2, net_server_data_temp.m_identity, net_server_data_temp.m_code, net_server_data_temp.m_data ); // 2
		}
	}
	catch( ... ) {
		std::string log_info = "ת���������� ����δ֪����";
		LogPrint( basicx::syslog_level::c_error, m_log_cate, log_info );
	}
}

int32_t QuoterCTP_P::gzCompress( Bytef* data_in, uLong size_in, Bytef* data_out, uLong* size_out, int32_t level ) {
	int err = 0;
	z_stream c_stream;

	if( data_in && size_in > 0 ) {
		c_stream.opaque = (voidpf)0;
		c_stream.zfree = (free_func)0;
		c_stream.zalloc = (alloc_func)0;

		// Ĭ�� level Ϊ Z_DEFAULT_COMPRESSION��windowBits ��Ϊ MAX_WBITS+16 ������ -MAX_WBITS������ѹ�������� WinRAR �ⲻ��
		if( deflateInit2( &c_stream, level, Z_DEFLATED, MAX_WBITS + 16, 8, Z_DEFAULT_STRATEGY ) != Z_OK ) {
			return -1;
		}

		c_stream.next_in = data_in;
		c_stream.avail_in = size_in;
		c_stream.next_out = data_out;
		c_stream.avail_out = *size_out;

		while( c_stream.avail_in != 0 && c_stream.total_out < *size_out ) {
			if( deflate( &c_stream, Z_NO_FLUSH ) != Z_OK ) {
				return -1;
			}
		}

		if( c_stream.avail_in != 0 ) {
			return c_stream.avail_in;
		}

		for( ; ; ) {
			if( ( err = deflate( &c_stream, Z_FINISH ) ) == Z_STREAM_END ) {
				break;
			}
			if( err != Z_OK ) {
				return -1;
			}
		}

		if( deflateEnd( &c_stream ) != Z_OK ) {
			return -1;
		}

		*size_out = c_stream.total_out;

		return 0;
	}

	return -1;
}

void QuoterCTP_P::HandleUserRequest() {
	std::string log_info;

	log_info = "�����û��������߳����, ��ʼ�����û������� ...";
	LogPrint( basicx::syslog_level::c_info, m_log_cate, log_info );

	try {
		while( 1 ) {
			if( !m_list_user_request.empty() ) {
				std::string strResultData = "";
				Request* pRequest = &m_list_user_request.front();

				try {
					int function = 0;
					if( NW_MSG_CODE_JSON == pRequest->m_code ) {
						function = pRequest->m_req_json["function"].asInt();
					}

					// ֻ����Ϊ TD_FUNC_QUOTE_ADDSUB��TD_FUNC_QUOTE_DELSUB��
					switch( function ) {
					case TD_FUNC_QUOTE_ADDSUB: // ��������
						strResultData = OnUserAddSub( pRequest );
						break;
					case TD_FUNC_QUOTE_DELSUB: // �˶�����
						strResultData = OnUserDelSub( pRequest );
						break;
					}
				}
				catch( ... ) {
					FormatLibrary::StandardLibrary::FormatTo( log_info, "�������� {0} �û�����ʱ����δ֪����", pRequest->m_task_id );
					strResultData = OnErrorResult( 0, -1, log_info, pRequest->m_code );
				}

				CommitResult( pRequest->m_task_id, pRequest->m_identity, pRequest->m_code, strResultData );

				m_user_request_list_lock.lock();
				m_list_user_request.pop_front();
				m_user_request_list_lock.unlock();
			}

			Sleep( 1 );
		}
	} // try
	catch( ... ) {
		log_info = "�û��������̷߳���δ֪����";
		LogPrint( basicx::syslog_level::c_fatal, m_log_cate, log_info );
	}

	log_info = "�û��������߳��˳���";
	LogPrint( basicx::syslog_level::c_warn, m_log_cate, log_info );
}

std::string QuoterCTP_P::OnUserAddSub( Request* request ) {
	std::string log_info;

	int nQuoteType = 0;
	std::string strQuoteList = "";
	if( NW_MSG_CODE_JSON == request->m_code ) {
		nQuoteType = request->m_req_json["quote_type"].asInt();
		strQuoteList = request->m_req_json["quote_list"].asString();
	}
	if( 0 == nQuoteType ) {
		log_info = "���鶩��ʱ �������� �쳣��";
		return OnErrorResult( TD_FUNC_QUOTE_ADDSUB, -1, log_info, request->m_code );
	}

	basicx::ConnectInfo* connect_info = m_net_server_broad->Server_GetConnect( request->m_identity );
	if( nullptr == connect_info ) {
		FormatLibrary::StandardLibrary::FormatTo( log_info, "���鶩��ʱ ��ȡ {0} ������Ϣ �쳣��", request->m_identity );
		return OnErrorResult( TD_FUNC_QUOTE_ADDSUB, -1, log_info, request->m_code );
	}

	if( TD_FUNC_QUOTE_DATA_MARKET_FUTURE_NP == nQuoteType ) {
		if( "" == strQuoteList ) { // ����ȫ�г�
			AddConSubAll( m_csm_snapshot_future, request->m_identity, connect_info );
			ClearConSubOne( m_csm_snapshot_future, request->m_identity ); // �˶����е���֤ȯ
		}
		else { // ָ��֤ȯ
			if( IsConSubAll( m_csm_snapshot_future, request->m_identity ) == false ) { // δȫ�г�����
				std::vector<std::string> vecFilterTemp;
				boost::split( vecFilterTemp, strQuoteList, boost::is_any_of( ", " ), boost::token_compress_on );
				int nSizeTemp = vecFilterTemp.size();
				for( int i = 0; i < nSizeTemp; i++ ) {
					if( vecFilterTemp[i] != "" ) {
						AddConSubOne( m_csm_snapshot_future, vecFilterTemp[i], request->m_identity, connect_info );
					}
				}
			}
		}
	}
	else {
		FormatLibrary::StandardLibrary::FormatTo( log_info, "���鶩��ʱ �������� δ֪��{0}", nQuoteType );
		return OnErrorResult( TD_FUNC_QUOTE_ADDSUB, -1, log_info, request->m_code );
	}

	FormatLibrary::StandardLibrary::FormatTo( log_info, "���鶩�ĳɹ���{0}", nQuoteType );
	LogPrint( basicx::syslog_level::c_info, m_log_cate, log_info );

	if( NW_MSG_CODE_JSON == request->m_code ) {
		Json::Value jsResults;
		jsResults["ret_func"] = TD_FUNC_QUOTE_ADDSUB;
		jsResults["ret_code"] = 0;
		jsResults["ret_info"] = log_info;
		jsResults["ret_numb"] = 0;
		jsResults["ret_data"] = "";
		return m_json_writer.write( jsResults );
	}

	return "";
}

std::string QuoterCTP_P::OnUserDelSub( Request* request ) {
	std::string log_info;

	int nQuoteType = 0;
	std::string strQuoteList = "";
	if( NW_MSG_CODE_JSON == request->m_code ) {
		nQuoteType = request->m_req_json["quote_type"].asInt();
		strQuoteList = request->m_req_json["quote_list"].asString();
	}
	if( 0 == nQuoteType ) {
		log_info = "�����˶�ʱ �������� �쳣��";
		return OnErrorResult( TD_FUNC_QUOTE_DELSUB, -1, log_info, request->m_code );
	}

	if( TD_FUNC_QUOTE_DATA_MARKET_FUTURE_NP == nQuoteType ) {
		if( "" == strQuoteList ) { // �˶�ȫ�г�
			DelConSubAll( m_csm_snapshot_future, request->m_identity );
			ClearConSubOne( m_csm_snapshot_future, request->m_identity ); // �˶����е���֤ȯ
		}
		else { // ָ��֤ȯ
			if( IsConSubAll( m_csm_snapshot_future, request->m_identity ) == false ) { // δȫ�г����ģ��Ѷ���ȫ�г��Ļ������ܻ��е�֤ȯ����
				std::vector<std::string> vecFilterTemp;
				boost::split( vecFilterTemp, strQuoteList, boost::is_any_of( ", " ), boost::token_compress_on );
				int nSizeTemp = vecFilterTemp.size();
				for( int i = 0; i < nSizeTemp; i++ ) {
					if( vecFilterTemp[i] != "" ) {
						DelConSubOne( m_csm_snapshot_future, vecFilterTemp[i], request->m_identity );
					}
				}
			}
		}
	}
	else {
		FormatLibrary::StandardLibrary::FormatTo( log_info, "�����˶�ʱ �������� δ֪��{0}", nQuoteType );
		return OnErrorResult( TD_FUNC_QUOTE_DELSUB, -1, log_info, request->m_code );
	}

	FormatLibrary::StandardLibrary::FormatTo( log_info, "�����˶��ɹ���{0}", nQuoteType );
	LogPrint( basicx::syslog_level::c_info, m_log_cate, log_info );

	if( NW_MSG_CODE_JSON == request->m_code ) {
		Json::Value jsResults;
		jsResults["ret_func"] = TD_FUNC_QUOTE_DELSUB;
		jsResults["ret_code"] = 0;
		jsResults["ret_info"] = log_info;
		jsResults["ret_numb"] = 0;
		jsResults["ret_data"] = "";
		return m_json_writer.write( jsResults );
	}

	return "";
}

void QuoterCTP_P::AddConSubAll( ConSubMan& csm_con_sub_man, int32_t identity, basicx::ConnectInfo* connect_info ) {
	csm_con_sub_man.m_lock_con_sub_all.lock();
	csm_con_sub_man.m_map_con_sub_all[identity] = connect_info;
	csm_con_sub_man.m_lock_con_sub_all.unlock();

	//std::string log_info;
	//FormatLibrary::StandardLibrary::FormatTo( log_info, "���� ȫ�г���{0}", identity );
	//LogPrint( basicx::syslog_level::c_debug, m_log_cate, log_info );
}

void QuoterCTP_P::DelConSubAll( ConSubMan& csm_con_sub_man, int32_t identity ) {
	csm_con_sub_man.m_lock_con_sub_all.lock();
	csm_con_sub_man.m_map_con_sub_all.erase( identity );
	csm_con_sub_man.m_lock_con_sub_all.unlock();

	//std::string log_info;
	//FormatLibrary::StandardLibrary::FormatTo( log_info, "�˶� ȫ�г���{0}", identity );
	//LogPrint( basicx::syslog_level::c_debug, m_log_cate, log_info );
}

bool QuoterCTP_P::IsConSubAll( ConSubMan& csm_con_sub_man, int32_t identity ) {
	bool bIsAllSubAlready = false;
	std::map<int32_t, basicx::ConnectInfo*>::iterator itCSA;
	csm_con_sub_man.m_lock_con_sub_all.lock();
	itCSA = csm_con_sub_man.m_map_con_sub_all.find( identity );
	if( itCSA != csm_con_sub_man.m_map_con_sub_all.end() ) {
		bIsAllSubAlready = true;
	}
	csm_con_sub_man.m_lock_con_sub_all.unlock();
	return bIsAllSubAlready;
}

void QuoterCTP_P::AddConSubOne( ConSubMan& csm_con_sub_man, std::string& code, int32_t identity, basicx::ConnectInfo* connect_info ) {
	ConSubOne* pOneSubCon = nullptr;
	std::map<std::string, ConSubOne*>::iterator itCSO;
	csm_con_sub_man.m_lock_con_sub_one.lock();
	itCSO = csm_con_sub_man.m_map_con_sub_one.find( code );
	if( itCSO != csm_con_sub_man.m_map_con_sub_one.end() ) {
		pOneSubCon = itCSO->second;
	}
	else {
		pOneSubCon = new ConSubOne;
		csm_con_sub_man.m_map_con_sub_one[code] = pOneSubCon;
	}
	csm_con_sub_man.m_lock_con_sub_one.unlock();

	pOneSubCon->m_lock_con_sub_one.lock();
	pOneSubCon->m_map_identity[identity] = connect_info;
	pOneSubCon->m_lock_con_sub_one.unlock();

	//std::string log_info;
	//FormatLibrary::StandardLibrary::FormatTo( log_info, "���� ��֤ȯ��{0} {1}", code, identity );
	//LogPrint( basicx::syslog_level::c_debug, m_log_cate, log_info );
}

void QuoterCTP_P::DelConSubOne( ConSubMan& csm_con_sub_man, std::string& code, int32_t identity ) {
	ConSubOne* pOneSubCon = nullptr;
	std::map<std::string, ConSubOne*>::iterator itCSO;
	csm_con_sub_man.m_lock_con_sub_one.lock();
	itCSO = csm_con_sub_man.m_map_con_sub_one.find( code );
	if( itCSO != csm_con_sub_man.m_map_con_sub_one.end() ) {
		pOneSubCon = itCSO->second;
		pOneSubCon->m_lock_con_sub_one.lock();
		pOneSubCon->m_map_identity.erase( identity );
		pOneSubCon->m_lock_con_sub_one.unlock();
		if( pOneSubCon->m_map_identity.empty() ) {
			csm_con_sub_man.m_map_con_sub_one.erase( code );
			csm_con_sub_man.m_lock_one_sub_con_del.lock();
			csm_con_sub_man.m_list_one_sub_con_del.push_back( pOneSubCon ); //Ŀǰ������ɾ��
			csm_con_sub_man.m_lock_one_sub_con_del.unlock();
		}

		//std::string log_info;
		//FormatLibrary::StandardLibrary::FormatTo( log_info, "�˶� ��֤ȯ��{0} {1}", code, identity );
		//LogPrint( basicx::syslog_level::c_debug, m_log_cate, log_info );
	}
	csm_con_sub_man.m_lock_con_sub_one.unlock();
}

void QuoterCTP_P::ClearConSubOne( ConSubMan& csm_con_sub_man, int32_t identity ) {
	std::vector<std::string> vecDelKeyTemp;
	ConSubOne* pOneSubCon = nullptr;
	std::map<std::string, ConSubOne*>::iterator itCSO;
	csm_con_sub_man.m_lock_con_sub_one.lock();
	for( itCSO = csm_con_sub_man.m_map_con_sub_one.begin(); itCSO != csm_con_sub_man.m_map_con_sub_one.end(); itCSO++ ) {
		pOneSubCon = itCSO->second;
		pOneSubCon->m_lock_con_sub_one.lock();
		pOneSubCon->m_map_identity.erase( identity );
		pOneSubCon->m_lock_con_sub_one.unlock();
		if( pOneSubCon->m_map_identity.empty() ) {
			vecDelKeyTemp.push_back( itCSO->first );
			csm_con_sub_man.m_lock_one_sub_con_del.lock();
			csm_con_sub_man.m_list_one_sub_con_del.push_back( pOneSubCon ); // Ŀǰ������ɾ��
			csm_con_sub_man.m_lock_one_sub_con_del.unlock();
		}
	}
	for( int i = 0; i < (int)vecDelKeyTemp.size(); i++ ) {
		csm_con_sub_man.m_map_con_sub_one.erase( vecDelKeyTemp[i] );
	}
	csm_con_sub_man.m_lock_con_sub_one.unlock();
}

void QuoterCTP_P::ClearUnavailableConSub( ConSubMan& csm_con_sub_man, bool is_idle_time ) { // ���������Ѿ�ʧЧ�����Ӷ��ģ�һ���ڿ���ʱ����
	std::vector<int32_t> vecDelKeyCSA;
	std::map<int32_t, basicx::ConnectInfo*>::iterator itCSA;
	csm_con_sub_man.m_lock_con_sub_all.lock();
	for( itCSA = csm_con_sub_man.m_map_con_sub_all.begin(); itCSA != csm_con_sub_man.m_map_con_sub_all.end(); itCSA++ ) {
		if( false == m_net_server_broad->IsConnectAvailable( itCSA->second ) ) { //��ʧЧ
			vecDelKeyCSA.push_back( itCSA->first );
		}
	}
	for( int i = 0; i < (int)vecDelKeyCSA.size(); i++ ) {
		csm_con_sub_man.m_map_con_sub_all.erase( vecDelKeyCSA[i] );
	}
	csm_con_sub_man.m_lock_con_sub_all.unlock();

	std::vector<std::string> vecDelKeyCSO;
	ConSubOne* pOneSubCon = nullptr;
	std::map<std::string, ConSubOne*>::iterator itCSO;
	csm_con_sub_man.m_lock_con_sub_one.lock();
	for( itCSO = csm_con_sub_man.m_map_con_sub_one.begin(); itCSO != csm_con_sub_man.m_map_con_sub_one.end(); itCSO++ ) {
		pOneSubCon = itCSO->second;
		std::vector<int32_t> vecDelKeyID;
		std::map<int32_t, basicx::ConnectInfo*>::iterator itID;
		pOneSubCon->m_lock_con_sub_one.lock();
		for( itID = pOneSubCon->m_map_identity.begin(); itID != pOneSubCon->m_map_identity.end(); itID++ ) {
			if( false == m_net_server_broad->IsConnectAvailable( itID->second ) ) { // ��ʧЧ
				vecDelKeyID.push_back( itID->first );
			}
		}
		for( int i = 0; i < (int)vecDelKeyID.size(); i++ ) {
			pOneSubCon->m_map_identity.erase( vecDelKeyID[i] );
		}
		pOneSubCon->m_lock_con_sub_one.unlock();
		if( pOneSubCon->m_map_identity.empty() ) {
			vecDelKeyCSO.push_back( itCSO->first );
			csm_con_sub_man.m_lock_one_sub_con_del.lock();
			csm_con_sub_man.m_list_one_sub_con_del.push_back( pOneSubCon ); // Ŀǰ������ɾ��
			csm_con_sub_man.m_lock_one_sub_con_del.unlock();
		}
	}
	for( int i = 0; i < (int)vecDelKeyCSO.size(); i++ ) {
		csm_con_sub_man.m_map_con_sub_one.erase( vecDelKeyCSO[i] );
	}
	csm_con_sub_man.m_lock_con_sub_one.unlock();

	if( true == is_idle_time ) { // ���ȷ�����ڿ���ʱִ�У��� m_list_one_sub_con_del Ҳ��������
		std::list<ConSubOne*>::iterator itOSC_L;
		csm_con_sub_man.m_lock_one_sub_con_del.lock();
		for( itOSC_L = csm_con_sub_man.m_list_one_sub_con_del.begin(); itOSC_L != csm_con_sub_man.m_list_one_sub_con_del.end(); itOSC_L++ ) {
			if( (*itOSC_L) != nullptr ) {
				delete (*itOSC_L);
			}
		}
		csm_con_sub_man.m_list_one_sub_con_del.clear();
		csm_con_sub_man.m_lock_one_sub_con_del.unlock();
	}
}

void QuoterCTP_P::CommitResult( int32_t task_id, int32_t identity, int32_t code, std::string& data ) {
	int nNetFlag = task_id % 10;
	if( 1 == nNetFlag ) { // Broad��1
		basicx::ConnectInfo* connect_info = m_net_server_broad->Server_GetConnect( identity );
		if( connect_info != nullptr ) {
			m_net_server_broad->Server_SendData( connect_info, NW_MSG_TYPE_USER_DATA, code, data );
		}
	}
	else if( 2 == nNetFlag ) { // Query��2
		basicx::ConnectInfo* connect_info = m_net_server_query->Server_GetConnect( identity );
		if( connect_info != nullptr ) {
			m_net_server_query->Server_SendData( connect_info, NW_MSG_TYPE_USER_DATA, code, data );
		}
	}
}

std::string QuoterCTP_P::OnErrorResult( int32_t ret_func, int32_t ret_code, std::string ret_info, int32_t encode ) {
	LogPrint( basicx::syslog_level::c_error, m_log_cate, ret_info );

	if( NW_MSG_CODE_JSON == encode ) {
		Json::Value jsResults;
		jsResults["ret_func"] = ret_func;
		jsResults["ret_code"] = ret_code;
		jsResults["ret_info"] = ret_info;
		jsResults["ret_numb"] = 0;
		jsResults["ret_data"] = "";
		return m_json_writer.write( jsResults );
	}

	return "";
}

// �Զ��� QuoterCTP ����ʵ��

CThostFtdcMdSpiImpl::CThostFtdcMdSpiImpl( CThostFtdcMdApi* user_api, QuoterCTP_P* quoter_ctp_p )
	: m_user_api( user_api )
	, m_quoter_ctp_p( quoter_ctp_p )
	, m_log_cate( "<Quoter_CTP>" ) {
}

CThostFtdcMdSpiImpl::~CThostFtdcMdSpiImpl() {
}

void CThostFtdcMdSpiImpl::OnFrontConnected() {
	std::string log_info;

	log_info = "���ӳɹ������������¼ ....";
	m_quoter_ctp_p->LogPrint( basicx::syslog_level::c_info, m_log_cate, log_info );

	CThostFtdcReqUserLoginField reqUserLogin;
	memset( &reqUserLogin, 0, sizeof( reqUserLogin ) );
	strcpy_s( reqUserLogin.BrokerID, m_quoter_ctp_p->m_configs.m_strbroker_id.c_str() );
	strcpy_s( reqUserLogin.UserID, m_quoter_ctp_p->m_configs.m_username.c_str() );
	strcpy_s( reqUserLogin.Password, m_quoter_ctp_p->m_configs.m_password.c_str() );
	int nRet = m_user_api->ReqUserLogin( &reqUserLogin, 0 );
	if( 0 == nRet ) {
		log_info = "�������� ��¼ ����ɹ���";
		m_quoter_ctp_p->LogPrint( basicx::syslog_level::c_info, m_log_cate, log_info );
	}
	else {
		log_info = "�������� ��¼ ����ʧ�ܣ�";
		m_quoter_ctp_p->LogPrint( basicx::syslog_level::c_error, m_log_cate, log_info );
	}
}

void CThostFtdcMdSpiImpl::OnFrontDisconnected( int nReason ) {
	// ��������������API���Զ��������ӣ��ͻ��˿ɲ�������
	std::string log_info;
	FormatLibrary::StandardLibrary::FormatTo( log_info, "���ӶϿ���Reason:{0}", nReason );
	m_quoter_ctp_p->LogPrint( basicx::syslog_level::c_warn, m_log_cate, log_info );
}

void CThostFtdcMdSpiImpl::OnHeartBeatWarning( int nTimeLapse ) {
	std::string log_info;
	FormatLibrary::StandardLibrary::FormatTo( log_info, "�������棺TimeLapse:{0}", nTimeLapse );
	m_quoter_ctp_p->LogPrint( basicx::syslog_level::c_info, m_log_cate, log_info );
}

void CThostFtdcMdSpiImpl::OnRspUserLogin( CThostFtdcRspUserLoginField* pRspUserLogin, CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast ) {
	std::string log_info;
	FormatLibrary::StandardLibrary::FormatTo( log_info, "��¼������ErrorID:{0} ErrorMsg:{1} RequestID:{2} Chain:{3}", pRspInfo->ErrorID, pRspInfo->ErrorMsg, nRequestID, bIsLast );
	m_quoter_ctp_p->LogPrint( basicx::syslog_level::c_info, m_log_cate, log_info );

	if( pRspInfo->ErrorID != 0 ) { // �����Ƿ�������Ժ����صǣ�
		log_info = "�����¼ʧ�ܣ�";
		m_quoter_ctp_p->LogPrint( basicx::syslog_level::c_error, m_log_cate, log_info );
	}
	else {
		FormatLibrary::StandardLibrary::FormatTo( log_info, "�����¼�ɹ�����ǰ�����գ�{0}���������鶩�� ....", pRspUserLogin->TradingDay );
		m_quoter_ctp_p->LogPrint( basicx::syslog_level::c_info, m_log_cate, log_info );

		// ָ�� JYDB �� CTP �Ὣ m_vec_contract ˢ�£�ָ�� USER �� m_vec_contract ����
		if( "JYDB" == m_quoter_ctp_p->m_configs.m_sub_list_from ) {
			m_quoter_ctp_p->GetSubListFromDB();
		}
		else if( "CTP" == m_quoter_ctp_p->m_configs.m_sub_list_from ) {
			m_quoter_ctp_p->GetSubListFromCTP();
		}

		if( 0 == m_quoter_ctp_p->m_vec_contract.size() ) {
			log_info = "��Ҫ��������ĺ�Լ����Ϊ�㣡";
			m_quoter_ctp_p->LogPrint( basicx::syslog_level::c_error, m_log_cate, log_info );
			return;
		}

		// ����Ͳ����˶���
		int nContractNum = m_quoter_ctp_p->m_vec_contract.size();
		char** ppcContract = new char*[nContractNum];
		for( int i = 0; i < nContractNum; i++ ) {
			ppcContract[i] = const_cast<char*>(m_quoter_ctp_p->m_vec_contract[i].c_str());
		}

		int nRet = m_user_api->SubscribeMarketData( ppcContract, nContractNum );
		if( 0 == nRet ) {
			m_quoter_ctp_p->m_subscribe_ok = true;
			log_info = "�������� ���� ����ɹ���";
			m_quoter_ctp_p->LogPrint( basicx::syslog_level::c_info, m_log_cate, log_info );
		}
		else {
			m_quoter_ctp_p->m_subscribe_ok = false;
			log_info = "�������� ���� ����ʧ�ܣ�";
			m_quoter_ctp_p->LogPrint( basicx::syslog_level::c_error, m_log_cate, log_info );
		}

		delete [] ppcContract;
	}
}

void CThostFtdcMdSpiImpl::OnRspUserLogout( CThostFtdcUserLogoutField* pUserLogout, CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast ) {
	std::string log_info;
	FormatLibrary::StandardLibrary::FormatTo( log_info, "�ǳ�������ErrorID:{0} ErrorMsg:{1} RequestID:{2} Chain:{3}", pRspInfo->ErrorID, pRspInfo->ErrorMsg, nRequestID, bIsLast );
	m_quoter_ctp_p->LogPrint( basicx::syslog_level::c_info, m_log_cate, log_info );
}

void CThostFtdcMdSpiImpl::OnRspError( CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast ) {
	std::string log_info;
	FormatLibrary::StandardLibrary::FormatTo( log_info, "�쳣������ErrorID:{0} ErrorMsg:{1} RequestID:{2} Chain:{3}", pRspInfo->ErrorID, pRspInfo->ErrorMsg, nRequestID, bIsLast );
	m_quoter_ctp_p->LogPrint( basicx::syslog_level::c_warn, m_log_cate, log_info );
}

void CThostFtdcMdSpiImpl::OnRspSubMarketData( CThostFtdcSpecificInstrumentField* pSpecificInstrument, CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast ) {
	std::string log_info;
	FormatLibrary::StandardLibrary::FormatTo( log_info, "���ķ�����ErrorID:{0} ErrorMsg:{1} RequestID:{2} Chain:{3}", pRspInfo->ErrorID, pRspInfo->ErrorMsg, nRequestID, bIsLast );
	m_quoter_ctp_p->LogPrint( basicx::syslog_level::c_info, m_log_cate, log_info, FILE_LOG_ONLY );
	m_quoter_ctp_p->m_subscribe_count++; // ��������������Ƿ��ĳɹ�
}

void CThostFtdcMdSpiImpl::OnRspUnSubMarketData( CThostFtdcSpecificInstrumentField* pSpecificInstrument, CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast ) {
	std::string log_info;
	FormatLibrary::StandardLibrary::FormatTo( log_info, "�˶�������ErrorID:{0} ErrorMsg:{1} RequestID:{2} Chain:{3}", pRspInfo->ErrorID, pRspInfo->ErrorMsg, nRequestID, bIsLast );
	m_quoter_ctp_p->LogPrint( basicx::syslog_level::c_info, m_log_cate, log_info, FILE_LOG_ONLY );
	m_quoter_ctp_p->m_subscribe_count--; // ��������������Ƿ��˶��ɹ�
}

void CThostFtdcMdSpiImpl::OnRtnDepthMarketData( CThostFtdcDepthMarketDataField* pDepthMarketData ) {
	SYSTEMTIME lpSysTime;
	GetLocalTime( &lpSysTime );
	tm tmNowTime = basicx::GetNowTime();
	char cNowDateTemp[9];
	strftime( cNowDateTemp, 9, "%Y%m%d", &tmNowTime);

	SnapshotFuture snapshot_future_temp;
	memset( &snapshot_future_temp, 0, sizeof( SnapshotFuture ) );

	strcpy_s( snapshot_future_temp.m_code, pDepthMarketData->InstrumentID ); // ��Լ����
	strcpy_s( snapshot_future_temp.m_name, "" ); // ��Լ���� // ��
	strcpy_s( snapshot_future_temp.m_type, "F" ); // ��Լ����
	strcpy_s( snapshot_future_temp.m_market, pDepthMarketData->ExchangeID ); // ��Լ�г�
	strcpy_s( snapshot_future_temp.m_status, "N" ); // ��Լ״̬������
	snapshot_future_temp.m_last = (uint32_t)(pDepthMarketData->LastPrice * 10000); // ���¼� // 10000
	snapshot_future_temp.m_open = (uint32_t)(pDepthMarketData->OpenPrice * 10000); // ���̼� // 10000
	snapshot_future_temp.m_high = (uint32_t)(pDepthMarketData->HighestPrice * 10000); // ��߼� // 10000
	snapshot_future_temp.m_low = (uint32_t)(pDepthMarketData->LowestPrice * 10000); // ��ͼ� // 10000
	snapshot_future_temp.m_close = (uint32_t)(pDepthMarketData->ClosePrice * 10000); // ���̼� // 10000
	snapshot_future_temp.m_pre_close = (uint32_t)(pDepthMarketData->PreClosePrice * 10000); // ���ռ� // 10000
	snapshot_future_temp.m_volume = pDepthMarketData->Volume; // �ɽ���
	snapshot_future_temp.m_turnover = (int64_t)(pDepthMarketData->Turnover * 10000); // �ɽ��� // 10000
	snapshot_future_temp.m_ask_price[0] = (uint32_t)(pDepthMarketData->AskPrice1 * 10000); // ���� 1 // 10000
	snapshot_future_temp.m_ask_volume[0] = pDepthMarketData->AskVolume1; // ���� 1
	snapshot_future_temp.m_ask_price[1] = (uint32_t)(pDepthMarketData->AskPrice2 * 10000); // ���� 2 // 10000
	snapshot_future_temp.m_ask_volume[1] = pDepthMarketData->AskVolume2; // ���� 2
	snapshot_future_temp.m_ask_price[2] = (uint32_t)(pDepthMarketData->AskPrice3 * 10000); // ���� 3 // 10000
	snapshot_future_temp.m_ask_volume[2] = pDepthMarketData->AskVolume3; // ���� 3
	snapshot_future_temp.m_ask_price[3] = (uint32_t)(pDepthMarketData->AskPrice4 * 10000); // ���� 4 // 10000
	snapshot_future_temp.m_ask_volume[3] = pDepthMarketData->AskVolume4; // ���� 4
	snapshot_future_temp.m_ask_price[4] = (uint32_t)(pDepthMarketData->AskPrice5 * 10000); // ���� 5 // 10000
	snapshot_future_temp.m_ask_volume[4] = pDepthMarketData->AskVolume5; // ���� 5
	snapshot_future_temp.m_bid_price[0] = (uint32_t)(pDepthMarketData->BidPrice1 * 10000); // ��� 1 // 10000
	snapshot_future_temp.m_bid_volume[0] = pDepthMarketData->BidVolume1; // ���� 1
	snapshot_future_temp.m_bid_price[1] = (uint32_t)(pDepthMarketData->BidPrice2 * 10000); // ��� 2 // 10000
	snapshot_future_temp.m_bid_volume[1] = pDepthMarketData->BidVolume2; // ���� 2
	snapshot_future_temp.m_bid_price[2] = (uint32_t)(pDepthMarketData->BidPrice3 * 10000); // ��� 3 // 10000
	snapshot_future_temp.m_bid_volume[2] = pDepthMarketData->BidVolume3; // ���� 3
	snapshot_future_temp.m_bid_price[3] = (uint32_t)(pDepthMarketData->BidPrice4 * 10000); // ��� 4 // 10000
	snapshot_future_temp.m_bid_volume[3] = pDepthMarketData->BidVolume4; // ���� 4
	snapshot_future_temp.m_bid_price[4] = (uint32_t)(pDepthMarketData->BidPrice5 * 10000); // ��� 5 // 10000
	snapshot_future_temp.m_bid_volume[4] = pDepthMarketData->BidVolume5; // ���� 5
	snapshot_future_temp.m_high_limit = (uint32_t)(pDepthMarketData->UpperLimitPrice * 10000); // ��ͣ�� // 10000
	snapshot_future_temp.m_low_limit = (uint32_t)(pDepthMarketData->LowerLimitPrice * 10000); // ��ͣ�� // 10000
	snapshot_future_temp.m_settle = (uint32_t)(pDepthMarketData->SettlementPrice * 10000); // ���ս���� // 10000
	snapshot_future_temp.m_pre_settle = (uint32_t)(pDepthMarketData->PreSettlementPrice * 10000); // ���ս���� // 10000
	snapshot_future_temp.m_position = (int32_t)pDepthMarketData->OpenInterest; // ���ճֲ���
	snapshot_future_temp.m_pre_position = (int32_t)pDepthMarketData->PreOpenInterest; // ���ճֲ���
	snapshot_future_temp.m_average = (uint32_t)(pDepthMarketData->AveragePrice * 10000); // ���� // 10000
	snapshot_future_temp.m_up_down = 0; // �ǵ� // 10000 // ��
	snapshot_future_temp.m_up_down_rate = 0; // �ǵ����� // 10000 // ��
	snapshot_future_temp.m_swing = 0; // ��� // 10000 // ��
	snapshot_future_temp.m_delta = (int32_t)(pDepthMarketData->CurrDelta * 10000); // ������ʵ�� // 10000
	snapshot_future_temp.m_pre_delta = (int32_t)(pDepthMarketData->PreDelta * 10000); // ������ʵ�� // 10000
	snapshot_future_temp.m_quote_date = atoi( pDepthMarketData->TradingDay ); // �������� // YYYYMMDD
	std::string quote_time_temp = "";
//	quote_time_temp.Format( "%s%03d", pDepthMarketData->UpdateTime, pDepthMarketData->UpdateMillisec ); // HH:MM:SSmmm
	basicx::StringReplace( quote_time_temp, ":", "" );
	snapshot_future_temp.m_quote_time = atoi( quote_time_temp.c_str() ); // ����ʱ�� // HHMMSSmmm ���ȣ�����
	snapshot_future_temp.m_local_date = ( tmNowTime.tm_year + 1900 ) * 10000 + ( tmNowTime.tm_mon + 1 ) * 100 + tmNowTime.tm_mday; // �������� // YYYYMMDD
	snapshot_future_temp.m_local_time = tmNowTime.tm_hour * 10000000 + tmNowTime.tm_min * 100000 + tmNowTime.tm_sec * 1000 + lpSysTime.wMilliseconds; // ����ʱ�� // HHMMSSmmm ���ȣ�����
	m_quoter_ctp_p->m_cache_snapshot_future.m_local_index++;
	snapshot_future_temp.m_local_index = m_quoter_ctp_p->m_cache_snapshot_future.m_local_index; // �������

	m_quoter_ctp_p->m_cache_snapshot_future.m_recv_num++;

	m_quoter_ctp_p->MS_AddData_SnapshotFuture( snapshot_future_temp );

	//std::string log_info;
	//FormatLibrary::StandardLibrary::FormatTo( log_info, "���鷴������Լ:{0} �г�:{1} �ּ�:{2} ��߼�:{3} ��ͼ�:{4} ��һ��:{5} ��һ��:{6} ��һ��:{7} ��һ��:{8}", 
	//	pDepthMarketData->InstrumentID, pDepthMarketData->ExchangeID, pDepthMarketData->LastPrice, pDepthMarketData->HighestPrice, pDepthMarketData->LowestPrice, 
	//	pDepthMarketData->AskPrice1, pDepthMarketData->AskVolume1, pDepthMarketData->BidPrice1, pDepthMarketData->BidVolume1 );
	//m_quoter_ctp_p->LogPrint( basicx::syslog_level::c_debug, m_log_cate, log_info );
}

CThostFtdcTraderSpiImpl::CThostFtdcTraderSpiImpl( CThostFtdcTraderApi* user_api, QuoterCTP_P* quoter_ctp_p )
	: m_user_api( user_api )
	, m_quoter_ctp_p( quoter_ctp_p )
	, m_log_cate( "<Quoter_CTP>" )
	, m_last_rsp_is_error( false )
	, m_last_error_msg( "�޴�����Ϣ��" )
	, m_request_id( 0 )
	, m_connect_ok( false )
	, m_login_ok( false )
	, m_qry_instrument( false )
	, m_logout_ok( false )
	, m_front_id( 0 )
	, m_session_id( 0 ) {
}

CThostFtdcTraderSpiImpl::~CThostFtdcTraderSpiImpl() {
}

void CThostFtdcTraderSpiImpl::OnFrontConnected() {
	m_connect_ok = true;

	std::string log_info;
	log_info = "����ǰ�����ӳɹ���";
	m_quoter_ctp_p->LogPrint( basicx::syslog_level::c_info, m_log_cate, log_info );
}

void CThostFtdcTraderSpiImpl::OnFrontDisconnected( int nReason ) { // CTP��API���Զ���������
	m_connect_ok = false;

	std::string log_info;
	FormatLibrary::StandardLibrary::FormatTo( log_info, "����ǰ�������жϣ�{0}", nReason );
	m_quoter_ctp_p->LogPrint( basicx::syslog_level::c_warn, m_log_cate, log_info );
}

void CThostFtdcTraderSpiImpl::OnHeartBeatWarning( int nTimeLapse ) {
	std::string log_info;
	FormatLibrary::StandardLibrary::FormatTo( log_info, "������ʱ���棡{0}", nTimeLapse );
	m_quoter_ctp_p->LogPrint( basicx::syslog_level::c_warn, m_log_cate, log_info );
}

int32_t CThostFtdcTraderSpiImpl::ReqUserLogin( std::string broker_id, std::string user_id, std::string password ) {
	CThostFtdcReqUserLoginField Req;
	memset( &Req, 0, sizeof( Req ) );
	strcpy_s( Req.BrokerID, const_cast<char*>(broker_id.c_str()) ); // ���͹�˾���� char 11
	strcpy_s( Req.UserID, const_cast<char*>(user_id.c_str()) ); // �û����� char 16
	strcpy_s( Req.Password, const_cast<char*>(password.c_str()) ); // ���� char 41
	//Req.TradingDay; // ������ char 9
	//Req.UserProductInfo; // �û��˲�Ʒ��Ϣ char 11
	//Req.InterfaceProductInfo; // �ӿڶ˲�Ʒ��Ϣ char 11
	//Req.ProtocolInfo; // Э����Ϣ char 11
	//Req.MacAddress; // Mac��ַ char 21
	//Req.OneTimePassword; // ��̬���� char 41
	//Req.ClientIPAddress; // �ն�IP��ַ char 16
	m_login_ok = false; // ֻ�����״ε�¼�ɹ����
	m_last_rsp_is_error = false;
	int nRequestID = GetRequestID();
	return m_user_api->ReqUserLogin( &Req, nRequestID );
}

void CThostFtdcTraderSpiImpl::OnRspUserLogin( CThostFtdcRspUserLoginField* pRspUserLogin, CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast ) {
	std::string log_info;
	if( ( pRspInfo && pRspInfo->ErrorID != 0 ) || !pRspUserLogin ) {
		m_last_error_msg = pRspInfo->ErrorMsg;
		m_last_rsp_is_error = true;
		FormatLibrary::StandardLibrary::FormatTo( log_info, "�û���¼ʧ�ܣ�{0}", m_last_error_msg );
		m_quoter_ctp_p->LogPrint( basicx::syslog_level::c_error, m_log_cate, log_info );
	}
	else {
		m_login_ok = true; // ֻ�����״ε�¼�ɹ����
		m_front_id = pRspUserLogin->FrontID; // ǰ�ñ�� int
		m_session_id = pRspUserLogin->SessionID; // �Ự��� int
		//m_pTraderCTP_P->m_nOrderRef = atoi( pRspUserLogin->MaxOrderRef ); // ��󱨵����� char 13 // ÿ���½��Ự CTP �������� OrderRef Ϊ1�������������Լ�����Ϊ׼
		//pRspUserLogin->TradingDay; // ������ char 9
		//pRspUserLogin->LoginTime; // ��¼�ɹ�ʱ�� char 9
		//pRspUserLogin->BrokerID; // ���͹�˾���� char 11
		//pRspUserLogin->UserID; // �û����� char 16
		//pRspUserLogin->SystemName; // ����ϵͳ���� char 41
		//pRspUserLogin->SHFETime; // ������ʱ�� char 9
		//pRspUserLogin->DCETime; // ������ʱ�� char 9
		//pRspUserLogin->CZCETime; // ֣����ʱ�� char 9
		//pRspUserLogin->FFEXTime; // �н���ʱ�� char 9
		FormatLibrary::StandardLibrary::FormatTo( log_info, "�û���¼�ɹ�����ǰ�����գ�{0}", pRspUserLogin->TradingDay );
		m_quoter_ctp_p->LogPrint( basicx::syslog_level::c_info, m_log_cate, log_info );
	}
}

int32_t CThostFtdcTraderSpiImpl::ReqUserLogout( std::string broker_id, std::string user_id ) {
	CThostFtdcUserLogoutField Req;
	memset( &Req, 0, sizeof( Req ) );
	strcpy_s( Req.BrokerID, const_cast<char*>(broker_id.c_str()) ); // ���͹�˾���� char 11
	strcpy_s( Req.UserID, const_cast<char*>(user_id.c_str()) ); // �û����� char 16
	m_logout_ok = false; // ֻ����ĩ�εǳ��ɹ����
	m_last_rsp_is_error = false;
	int nRequestID = GetRequestID();
	return m_user_api->ReqUserLogout( &Req, nRequestID );
}

void CThostFtdcTraderSpiImpl::OnRspUserLogout( CThostFtdcUserLogoutField* pUserLogout, CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast ) {
	std::string log_info;
	if( ( pRspInfo && pRspInfo->ErrorID != 0 ) || !pUserLogout ) {
		m_last_error_msg = pRspInfo->ErrorMsg;
		m_last_rsp_is_error = true;
		FormatLibrary::StandardLibrary::FormatTo( log_info, "�û��ǳ�ʧ�ܣ�{0}", m_last_error_msg );
		m_quoter_ctp_p->LogPrint( basicx::syslog_level::c_error, m_log_cate, log_info );
	}
	else {
		m_logout_ok = true; // ֻ����ĩ�εǳ��ɹ����
		//pUserLogout->BrokerID; // ���͹�˾���� char 11
		//pUserLogout->UserID; // �û����� char 16
		FormatLibrary::StandardLibrary::FormatTo( log_info, "�û��ǳ��ɹ���{0} {1}", pUserLogout->BrokerID, pUserLogout->UserID );
		m_quoter_ctp_p->LogPrint( basicx::syslog_level::c_info, m_log_cate, log_info );
	}
}

int32_t CThostFtdcTraderSpiImpl::ReqQryInstrument() {
	CThostFtdcQryInstrumentField Req;
	memset( &Req, 0, sizeof( Req ) );
	//Req.InstrumentID; // ��Լ���� char 31 // Ϊ�ձ�ʾ��ѯ���к�Լ
	//Req.ExchangeID; // ���������� char 9
	//Req.ExchangeInstID; // ��Լ�ڽ������Ĵ��� char 31
	//Req.ProductID; // ��Ʒ���� char 31
	m_qry_instrument = false; //
	int nRequestID = GetRequestID();
	return m_user_api->ReqQryInstrument( &Req, nRequestID );
}

void CThostFtdcTraderSpiImpl::OnRspQryInstrument( CThostFtdcInstrumentField* pInstrument, CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast ) {
	std::string log_info;
	
	if( ( pRspInfo && pRspInfo->ErrorID != 0 ) || !pInstrument ) {
		if( pRspInfo ) {
			FormatLibrary::StandardLibrary::FormatTo( log_info, "�ڻ���Լ��ѯ�ύʧ�ܣ�{0}", pRspInfo->ErrorMsg );
		}
		else {
			log_info = "�ڻ���Լ��ѯ�ύʧ�ܣ�ԭ��δ֪��";
		}
		m_quoter_ctp_p->LogPrint( basicx::syslog_level::c_error, m_log_cate, log_info );
	}
	else {
		if( true == bIsLast ) { //
			m_qry_instrument = true;
		}

		std::string contract_code = pInstrument->InstrumentID; // ��Լ���� char 31
		std::string strExchangeID = pInstrument->ExchangeID; // ���������� char 9
		if( contract_code != "" && contract_code.length() < 8 ) { // ��Щ���Ի���(�����ڻ�)���ѯ������ IO1402-C-2550 �����Ĵ��뵼���������ʱ�����ֶ��������
			m_quoter_ctp_p->m_vec_contract.push_back( contract_code );
			FormatLibrary::StandardLibrary::FormatTo( log_info, "��Լ���룺{0} {1}", contract_code, strExchangeID );
			m_quoter_ctp_p->LogPrint( basicx::syslog_level::c_info, m_log_cate, log_info, FILE_LOG_ONLY );
		}
	}
}

int32_t CThostFtdcTraderSpiImpl::GetRequestID() {
	m_request_id++;
	if( m_request_id > 2147483600 ) { // 2147483648
		m_request_id = 1;
	}
	return m_request_id;
}

std::string CThostFtdcTraderSpiImpl::GetLastErrorMsg() {
	std::string strLastErrorMsg = m_last_error_msg;
	m_last_error_msg = "�޴�����Ϣ��";
	return strLastErrorMsg;
}

// Ԥ�� QuoterCTP ����ʵ��

QuoterCTP::QuoterCTP()
	: basicx::Plugins_X( PLUGIN_NAME )
	, m_quoter_ctp_p( nullptr ) {
	try {
		m_quoter_ctp_p = new QuoterCTP_P();
	}
	catch( ... ) {}
}

QuoterCTP::~QuoterCTP() {
	if( m_quoter_ctp_p != nullptr ) {
		delete m_quoter_ctp_p;
		m_quoter_ctp_p = nullptr;
	}
}

bool QuoterCTP::Initialize() {
	return m_quoter_ctp_p->Initialize();
}

bool QuoterCTP::InitializeExt() {
	return m_quoter_ctp_p->InitializeExt();
}

bool QuoterCTP::StartPlugin() {
	return m_quoter_ctp_p->StartPlugin();
}

bool QuoterCTP::IsPluginRun() {
	return m_quoter_ctp_p->IsPluginRun();
}

bool QuoterCTP::StopPlugin() {
	return m_quoter_ctp_p->StopPlugin();
}

bool QuoterCTP::UninitializeExt() {
	return m_quoter_ctp_p->UninitializeExt();
}

bool QuoterCTP::Uninitialize() {
	return m_quoter_ctp_p->Uninitialize();
}

bool QuoterCTP::AssignTask( int32_t task_id, int32_t identity, int32_t code, std::string& data ) {
	return m_quoter_ctp_p->AssignTask( task_id, identity, code, data );
}

// �Զ��� QuoterCTP ����ʵ��
