﻿#include "ctp_trader.h"
#include <file_wapper.hpp>
#include <time_utils.hpp>

#ifdef _WIN32
#ifdef _WIN64
#pragma comment (lib,"../api/v6.6.9_traderapi_20220920/v6.6.9_20220914_winApi/traderapi/20220914_traderapi64_se_windows/thosttraderapi_se.lib")
#else
#pragma comment (lib,"../api/v6.6.9_traderapi_20220920/v6.6.9_20220914_winApi/traderapi/20220914_traderapi_se_windows/thosttraderapi_se.lib")
#endif
#endif
ctp_trader::ctp_trader()
	: _td_api(nullptr)
	, _reqid(0)
	, _last_query_time(0)
	, _front_id(0)
	, _session_id(0)
	, _order_ref(0)
	, _is_runing(false)
	, _process_mutex(_mutex)
	, _work_thread(nullptr)
	, _is_inited(false)
	, _is_connected(false)
	, _is_sync_wait(false)
{
}


ctp_trader::~ctp_trader()
{
	_is_runing = false;
	if (_work_thread)
	{
		_work_thread->join();
		delete _work_thread;
		_work_thread = nullptr;
	}
	if (_td_api)
	{
		_td_api->RegisterSpi(nullptr);
		_td_api->Release();
		//_td_api->Join();
		_td_api = nullptr;
	}
	_today_position.clear();
	_history_position.clear();
	_order_info.clear();
}

bool ctp_trader::init(const boost::property_tree::ptree& config)
{
	try
	{
		_front_addr = config.get<std::string>("front");
		_broker_id = config.get<std::string>("broker");
		_userid = config.get<std::string>("userid");
		_password = config.get<std::string>("passwd");
		_appid = config.get<std::string>("appid");
		_authcode = config.get<std::string>("authcode");
		_prodict_info = config.get<std::string>("prodict");
	}
	catch (...)
	{
		LOG_ERROR("ctp_trader init error ");
		return false;
	}
	
	char path_buff[64];
	sprintf(path_buff,"td_flow/%s/%s/", _broker_id.c_str(), _userid.c_str());
	if (!file_wapper::exists(path_buff))
	{
		file_wapper::create_directories(path_buff);
	}
	_td_api = CThostFtdcTraderApi::CreateFtdcTraderApi(path_buff);
	_td_api->RegisterSpi(this);
	//_td_api->SubscribePrivateTopic(THOST_TERT_RESTART);
	//_td_api->SubscribePublicTopic(THOST_TERT_RESTART);
	_td_api->SubscribePrivateTopic(THOST_TERT_RESUME);
	_td_api->SubscribePublicTopic(THOST_TERT_RESUME);
	_td_api->RegisterFront(const_cast<char*>(_front_addr.c_str()));
	_td_api->Init();
	LOG_INFO("ctp_trader init %s ", _td_api->GetApiVersion());
	_process_signal.wait(_process_mutex);
	_is_runing = true ;
	//启动查询线程去同步账户信息
	if (_work_thread == nullptr)
	{
		_work_thread = new std::thread([this]() {
			while (_is_runing)
			{
				if (!_query_queue.empty())
				{
					auto& handler = _query_queue.front();
					if (_is_in_query)
					{
						continue ;
					}
					while (!_is_in_query.exchange(true));
					handler();
					_query_mutex.lock();
					_query_queue.pop();
					_query_mutex.unlock();
					_last_query_time = get_now();
				}
				std::this_thread::sleep_for(std::chrono::seconds(1));
				//query_account();
			}
			});
		//_work_thread->join();
	}
	query_positions(true);
	query_orders(true);
	query_account(true);
	_is_inited = true ;
	return true;
}


bool ctp_trader::do_login()
{
	if (_td_api == nullptr)
	{
		return false;
	}
	CThostFtdcReqUserLoginField req;
	memset(&req, 0, sizeof(req));
	strcpy(req.BrokerID, _broker_id.c_str());
	strcpy(req.UserID, _userid.c_str());
	strcpy(req.Password, _password.c_str());
	strcpy(req.UserProductInfo, _prodict_info.c_str());
	int iResult = _td_api->ReqUserLogin(&req, genreqid());
	if (iResult != 0)
	{
		LOG_ERROR("ctp_trader do_login request failed: %d", iResult);
		return false ;
	}
	return true;
}

estid_t ctp_trader::do_order(offset_type offset, direction_type direction, const code_t& code, uint32_t count, double_t price, order_flag flag, bool is_close_history)
{
	estid_t est_id = generate_estid();

	CThostFtdcInputOrderField req;
	memset(&req, 0, sizeof(req));
	strcpy(req.BrokerID, _broker_id.c_str());
	strcpy(req.InvestorID, _userid.c_str());

	strcpy(req.InstrumentID, code.get_id());
	strcpy(req.ExchangeID, code.get_excg());

	uint32_t order_ref = 0, season_id = 0, front_id = 0;
	extract_estid(est_id, front_id, season_id, order_ref);
	///报单引用
	sprintf(req.OrderRef, "%u", order_ref);

	if (price != .0F)
	{
		///报单价格条件: 限价
		req.OrderPriceType = THOST_FTDC_OPT_LimitPrice;

	}
	else
	{
		req.OrderPriceType = THOST_FTDC_OPT_BestPrice;
	}
	///买卖方向: 
	req.Direction = wrap_direction_offset(direction, offset);
	///组合开平标志: 开仓
	req.CombOffsetFlag[0] = wrap_offset(code, offset, is_close_history);
	///组合投机套保标志
	req.CombHedgeFlag[0] = THOST_FTDC_HF_Speculation;
	///价格
	req.LimitPrice = price;
	///数量: 1
	req.VolumeTotalOriginal = count;

	if (flag == order_flag::OF_NOR)
	{
		req.TimeCondition = THOST_FTDC_TC_GFD;
		req.VolumeCondition = THOST_FTDC_VC_AV;
		req.MinVolume = 1;
	}
	else if (flag == order_flag::OF_FAK)
	{
		req.TimeCondition = THOST_FTDC_TC_IOC;
		req.VolumeCondition = THOST_FTDC_VC_AV;
		req.MinVolume = 1;
	}
	else if (flag == order_flag::OF_FOK)
	{
		req.TimeCondition = THOST_FTDC_TC_IOC;
		req.VolumeCondition = THOST_FTDC_VC_CV;
		req.MinVolume = count;
	}


	///触发条件: 立即
	req.ContingentCondition = THOST_FTDC_CC_Immediately;
	///强平原因: 非强平
	req.ForceCloseReason = THOST_FTDC_FCC_NotForceClose;
	///自动挂起标志: 否
	req.IsAutoSuspend = 0;
	///用户强评标志: 否
	req.UserForceClose = 0;

	int iResult = _td_api->ReqOrderInsert(&req, genreqid());
	if (iResult != 0)
	{
		LOG_ERROR("ctp_trader order_insert request failed: %d", iResult);
		return INVALID_ESTID;
	}
	LOG_INFO("ctp_trader place_order : %llu", est_id);
	return est_id;
}

bool ctp_trader::logout()
{
	if (_td_api == nullptr)
	{
		return false;
	}

	CThostFtdcUserLogoutField req;
	memset(&req, 0, sizeof(req));
	strcpy(req.BrokerID, _broker_id.c_str());
	strcpy(req.UserID, _userid.c_str());
	int iResult = _td_api->ReqUserLogout(&req, genreqid());
	if (iResult != 0)
	{
		LOG_ERROR("ctp_trader logout request failed: %d", iResult);
		return false ;
	}

	return true;
}


void ctp_trader::query_account(bool is_sync)
{
	if (_td_api == nullptr)
	{
		return ;
	}
	_query_mutex.lock();
	_query_queue.push([this]() {
		CThostFtdcQryTradingAccountField req;
		memset(&req, 0, sizeof(req));
		strcpy(req.BrokerID, _broker_id.c_str());
		strcpy(req.InvestorID, _userid.c_str());
		_td_api->ReqQryTradingAccount(&req, genreqid());
		});
	_query_mutex.unlock();
	if (is_sync)
	{
		while (!_is_sync_wait.exchange(true));
		_process_signal.wait(_process_mutex);
	}
}

void ctp_trader::query_positions(bool is_sync)
{
	if (_td_api == nullptr)
	{
		return;
	}

	_query_mutex.lock();
	
	_query_queue.push([this]() {
		
		CThostFtdcQryInvestorPositionField req;
		memset(&req, 0, sizeof(req));
		strcpy(req.BrokerID, _broker_id.c_str());
		strcpy(req.InvestorID, _userid.c_str());
		_td_api->ReqQryInvestorPosition(&req, genreqid());
	});
	
	_query_mutex.unlock();
	if(is_sync)
	{
		while (!_is_sync_wait.exchange(true));
		_process_signal.wait(_process_mutex);
	}
}

void ctp_trader::query_orders(bool is_sync)
{
	if (_td_api == nullptr)
	{
		return;
	}
	_query_mutex.lock();
	
	_query_queue.push([this]() {
		CThostFtdcQryOrderField req;
		memset(&req, 0, sizeof(req));
		strcpy(req.BrokerID, _broker_id.c_str());
		strcpy(req.InvestorID, _userid.c_str());
		_td_api->ReqQryOrder(&req, genreqid());
	});
	_query_mutex.unlock();
	if (is_sync)
	{
		while (!_is_sync_wait.exchange(true));
		_process_signal.wait(_process_mutex);
	}
}

void ctp_trader::query_trades(bool is_sync)
{
	if (_td_api == nullptr)
	{
		return;
	}

	_query_mutex.lock();

	_query_queue.push([this]() {
		CThostFtdcQryTradeField req;
		memset(&req, 0, sizeof(req));
		strcpy(req.BrokerID, _broker_id.c_str());
		strcpy(req.InvestorID, _userid.c_str());
		_td_api->ReqQryTrade(&req, genreqid());
	});
	_query_mutex.unlock();
	if (is_sync)
	{
		while (!_is_sync_wait.exchange(true));
		_process_signal.wait(_process_mutex);
	}
}

void ctp_trader::OnFrontConnected()
{
	LOG_INFO("ctp_trader OnFrontConnected ");
	do_auth();
	_is_connected = true ;
}

void ctp_trader::OnFrontDisconnected(int nReason)
{
	LOG_INFO("ctp_trader FrontDisconnected : Reason -> %d", nReason);
	_is_connected = false ;
}

void ctp_trader::OnRspAuthenticate(CThostFtdcRspAuthenticateField *pRspAuthenticateField, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast)
{
	if (pRspInfo && pRspInfo->ErrorID)
	{
		LOG_ERROR("ctp_trader OnRspAuthenticate Error : %d", pRspInfo->ErrorID);
		return ;
	}
	do_login();
}

void ctp_trader::OnRspUserLogin(CThostFtdcRspUserLoginField *pRspUserLogin, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast)
{
	if (pRspInfo && pRspInfo->ErrorID)
	{
		LOG_ERROR("ctp_trader OnRspUserLogin Error : %d", pRspInfo->ErrorID);
		return;
	}

	if (pRspUserLogin)
	{

		LOG_INFO("[%s][用户登录] UserID:%s AppID:%s SessionID:%d FrontID:%d\n",
			datetime_to_string(pRspUserLogin->TradingDay, pRspUserLogin->LoginTime).c_str(),
			pRspUserLogin->UserID, _appid.c_str(), pRspUserLogin->SessionID, pRspUserLogin->FrontID);
		//LOG("OnRspUserLogin\tErrorID = [%d] ErrorMsg = [%s]\n", pRspInfo->ErrorID, pRspInfo->ErrorMsg);
			// 保存会话参数
		_front_id = pRspUserLogin->FrontID;
		_session_id = pRspUserLogin->SessionID;
		_order_ref = atoi(pRspUserLogin->MaxOrderRef);
	}

	if (bIsLast&&!_is_inited)
	{
		_process_signal.notify_all();
	}

}

void ctp_trader::OnRspUserLogout(CThostFtdcUserLogoutField *pUserLogout, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast)
{
	if (pRspInfo)
	{
		LOG_DEBUG("UserLogout : %d -> %s", pRspInfo->ErrorID, pRspInfo->ErrorMsg);
	}
}

void ctp_trader::OnRspSettlementInfoConfirm(CThostFtdcSettlementInfoConfirmField *pSettlementInfoConfirm, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast)
{
	if (pRspInfo)
	{
		LOG_DEBUG("OnRspSettlementInfoConfirm\tErrorID = [%d] ErrorMsg = [%s]\n", pRspInfo->ErrorID, pRspInfo->ErrorMsg);
	}
	this->fire_event(trader_event_type::TET_SettlementCompleted);
}

void ctp_trader::OnRspOrderInsert(CThostFtdcInputOrderField *pInputOrder, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast)
{
	if (pRspInfo && pRspInfo->ErrorID != 0)
	{
		LOG_ERROR("OnRspOrderInsert \tErrorID = [%d] ErrorMsg = [%s]\n", pRspInfo->ErrorID, pRspInfo->ErrorMsg);
		print_position("OnRspOrderInsert");
	}
	if (pInputOrder && pRspInfo)
	{
		estid_t estid = generate_estid(_front_id, _session_id, strtol(pInputOrder->OrderRef, NULL, 10));
		this->fire_event(trader_event_type::TET_OrderError, error_type::ET_PLACE_ORDER, estid, (uint32_t)pRspInfo->ErrorID);
	}
}

void ctp_trader::OnRspOrderAction(CThostFtdcInputOrderActionField *pInputOrderAction, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast)
{
	if (pRspInfo && pRspInfo->ErrorID != 0)
	{
		LOG_ERROR("OnRspOrderAction \tErrorID = [%d] ErrorMsg = [%s]\n", pRspInfo->ErrorID, pRspInfo->ErrorMsg);
	}
	if (pInputOrderAction && pRspInfo)
	{
		estid_t estid = generate_estid(pInputOrderAction->FrontID, pInputOrderAction->SessionID, strtol(pInputOrderAction->OrderRef, NULL, 10));
		this->fire_event(trader_event_type::TET_OrderError, error_type::ET_CANCEL_ORDER, estid, (uint32_t)pRspInfo->ErrorID);
	}
}

void ctp_trader::OnRspQryTradingAccount(CThostFtdcTradingAccountField *pTradingAccount, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast)
{
	if (_is_in_query)
	{
		while(!_is_in_query.exchange(false));
	}
	if (pRspInfo && pRspInfo->ErrorID != 0)
	{
		LOG_ERROR("OnRspQryTradingAccount \tErrorID = [%d] ErrorMsg = [%s]\n", pRspInfo->ErrorID, pRspInfo->ErrorMsg);
		return;
	}
	
	if (bIsLast&& pTradingAccount)
	{
		double_t frozen = pTradingAccount->FrozenCommission + pTradingAccount->FrozenMargin + pTradingAccount->CurrMargin;
		if(_account_info.money != pTradingAccount->Balance || _account_info.frozen_monery != frozen)
		{
			_account_info.money = pTradingAccount->Balance;
			_account_info.frozen_monery = frozen;
			this->fire_event(trader_event_type::TET_AccountChange, _account_info);
		}
	}
	if (bIsLast && _is_sync_wait)
	{
		print_position("OnRspQryInvestorPosition");
		_process_signal.notify_all();
		while (!_is_sync_wait.exchange(false));
	}
}

void ctp_trader::OnRspQryInvestorPosition(CThostFtdcInvestorPositionField *pInvestorPosition, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast)
{
	if (_is_in_query)
	{
		while (!_is_in_query.exchange(false));
		_today_position.clear();
		_history_position.clear();
	}
	if (pRspInfo && pRspInfo->ErrorID != 0)
	{
		LOG_ERROR("OnRspQryInvestorPosition \tErrorID = [%d] ErrorMsg = [%s]\n", pRspInfo->ErrorID, pRspInfo->ErrorMsg);
		return;
	}
	
	if (pInvestorPosition)
	{
		LOG_DEBUG("OnRspQryInvestorPosition %s %d %d %d\n", pInvestorPosition->InstrumentID, pInvestorPosition->TodayPosition, pInvestorPosition->Position, pInvestorPosition->YdPosition);
		code_t code(pInvestorPosition->InstrumentID, pInvestorPosition->ExchangeID);
		if (code.is_split_position())
		{
			if (pInvestorPosition->PositionDate == THOST_FTDC_PSD_Today)
			{
				position_info& position = _today_position[code];
				position.id = code;
				if (pInvestorPosition->PosiDirection == THOST_FTDC_PD_Long)
				{
					position.long_cell.volume += pInvestorPosition->TodayPosition;
					position.long_cell.frozen += pInvestorPosition->ShortFrozen;
				}
				else
				{
					position.short_cell.volume += pInvestorPosition->TodayPosition;
					position.short_cell.frozen += pInvestorPosition->LongFrozen;
				}
			}
			else
			{
				position_info& position = _history_position[code];
				position.id = code;
				uint32_t yestoday_position = pInvestorPosition->Position - pInvestorPosition->TodayPosition;
				if (pInvestorPosition->PosiDirection == THOST_FTDC_PD_Long)
				{
					position.long_cell.volume += yestoday_position;
					position.long_cell.frozen += pInvestorPosition->ShortFrozen;
				}
				else
				{
					position.short_cell.volume += yestoday_position;
					position.short_cell.frozen += pInvestorPosition->LongFrozen;
				}
			}
		}
		else
		{
			position_info& position = _today_position[code];
			position.id = code;
			uint32_t history_position = pInvestorPosition->Position - pInvestorPosition->TodayPosition;
			if (pInvestorPosition->PosiDirection == THOST_FTDC_PD_Long)
			{
				position.long_cell.volume += pInvestorPosition->TodayPosition + history_position;
				position.long_cell.frozen += pInvestorPosition->ShortFrozen;
			}
			else
			{
				position.short_cell.volume += pInvestorPosition->TodayPosition + history_position;
				position.short_cell.frozen += pInvestorPosition->LongFrozen;
			}
		}
	}
	if (bIsLast && _is_sync_wait)
	{
		print_position("OnRspQryInvestorPosition");
		_process_signal.notify_all();
		while (!_is_sync_wait.exchange(false));
	}
}


void ctp_trader::OnRspQryTrade(CThostFtdcTradeField *pTrade, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast)
{
	if (_is_in_query)
	{
		while (!_is_in_query.exchange(false));
	}
	if (pRspInfo&& pRspInfo->ErrorID != 0)
	{
		LOG_ERROR("OnRspQryTrade \tErrorID = [%d] ErrorMsg = [%s]\n", pRspInfo->ErrorID, pRspInfo->ErrorMsg);
		return;
	}
	if (bIsLast && _is_sync_wait)
	{
		print_position("OnRspQryTrade");
		_process_signal.notify_all();
		while (!_is_sync_wait.exchange(false));
	}
}

void ctp_trader::OnRspQryOrder(CThostFtdcOrderField *pOrder, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast)
{
	if (_is_in_query)
	{
		while (!_is_in_query.exchange(false));
		_order_info.clear();
	}
	if (pOrder&& pOrder->VolumeTotal>0&& pOrder->OrderStatus!= THOST_FTDC_OST_Canceled&& pOrder->OrderStatus != THOST_FTDC_OST_AllTraded)
	{
		estid_t estid = generate_estid(pOrder->FrontID, pOrder->SessionID,strtoul(pOrder->OrderRef,NULL,10));
		auto order = _order_info[estid];
		order.code = code_t(pOrder->InstrumentID , pOrder->ExchangeID);
		order.create_time = make_datetime(pOrder->InsertDate, pOrder->InsertTime);
		order.est_id = estid;
		order.direction = convert_direction(pOrder->Direction);
		order.offset = convert_offset(pOrder->CombOffsetFlag[0]);
		order.last_volume = pOrder->VolumeTotal;
		order.total_volume = pOrder->VolumeTotal + pOrder->VolumeTraded;
		order.price = pOrder->LimitPrice;
		_order_info[estid] = order;
		LOG_INFO("OnRspQryOrder %s %lld %d %d %s %f\n", pOrder->InstrumentID, estid, pOrder->FrontID, pOrder->SessionID, pOrder->OrderRef, pOrder->LimitPrice);
	}

	if (bIsLast && _is_sync_wait)
	{
		print_position("OnRspQryTrade");
		_process_signal.notify_all();
		while (!_is_sync_wait.exchange(false));
	}
}

void ctp_trader::OnRspError(CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast)
{
	if(!_is_inited)
	{
		return ;
	}
	if (pRspInfo)
	{
		LOG_ERROR("OnRspError \tErrorID = [%d] ErrorMsg = [%s]\n", pRspInfo->ErrorID, pRspInfo->ErrorMsg);
		this->fire_event(trader_event_type::TET_OrderError,error_type::ET_OTHER_ERROR, INVALID_ESTID, (uint32_t)pRspInfo->ErrorID);
	}

}

void ctp_trader::OnRtnOrder(CThostFtdcOrderField *pOrder)
{
	if(pOrder == nullptr||! _is_inited)
	{
		return ;
	}

	auto estid = generate_estid(pOrder->FrontID, pOrder->SessionID, strtoul(pOrder->OrderRef, NULL, 10));
	auto code = code_t(pOrder->InstrumentID, pOrder->ExchangeID);
	auto direction = convert_direction_offset(pOrder->Direction, pOrder->CombOffsetFlag[0]);
	auto offset = convert_offset(pOrder->CombOffsetFlag[0]);
	auto is_today = (THOST_FTDC_OF_CloseToday == pOrder->CombOffsetFlag[0]);
	LOG_INFO("OnRtnOrder %llu %d %d %s %d %d %c \n", estid, pOrder->FrontID, pOrder->SessionID, pOrder->InstrumentID, direction, offset, pOrder->OrderStatus);

	if (pOrder->OrderStatus == THOST_FTDC_OST_Canceled || pOrder->OrderStatus == THOST_FTDC_OST_AllTraded)
	{
		auto it = _order_info.find(estid);
		if (it != _order_info.end())
		{	
			auto order = it->second;
			if (order.last_volume > static_cast<uint32_t>(pOrder->VolumeTotal))
			{
				calculate_position(code, direction, offset, order.last_volume - static_cast<uint32_t>(pOrder->VolumeTotal), pOrder->LimitPrice, is_today);
				order.last_volume = pOrder->VolumeTotal;
			}
			if (pOrder->OrderStatus == THOST_FTDC_OST_Canceled)
			{
				//撤销解冻仓位
				if (offset == offset_type::OT_CLOSE)
				{
					thawing_deduction(code, direction, pOrder->VolumeTotal + pOrder->VolumeTraded, is_today);
				}
				LOG_INFO("OnRtnOrder fire_event ET_OrderCancel %llu %s %d %d \n", estid, code.get_id(), direction, offset);
				this->fire_event(trader_event_type::TET_OrderCancel, estid, code, offset, direction, pOrder->LimitPrice, (uint32_t)pOrder->VolumeTotal, (uint32_t)(pOrder->VolumeTraded + pOrder->VolumeTotal));
			}
			if (pOrder->OrderStatus == THOST_FTDC_OST_AllTraded)
			{
				LOG_INFO("OnRtnOrder fire_event ET_OrderTrade %llu %s %d %d \n", estid, code.get_id(), direction, offset);
				this->fire_event(trader_event_type::TET_OrderTrade, estid, code, offset, direction, pOrder->LimitPrice, (uint32_t)(pOrder->VolumeTraded + pOrder->VolumeTotal));
			}
			_order_info.erase(it);
		}
	}
	else
	{
		auto it = _order_info.find(estid);
		if (it == _order_info.end())
		{
			order_info entrust;
			entrust.code = code;
			entrust.create_time = make_datetime(pOrder->InsertDate, pOrder->InsertTime);
			entrust.est_id = estid;
			entrust.direction = direction;
			entrust.last_volume = pOrder->VolumeTotal;
			entrust.total_volume = pOrder->VolumeTotal + pOrder->VolumeTraded;
			entrust.offset = offset;
			entrust.price = pOrder->LimitPrice;
			_order_info.insert(std::make_pair(estid, entrust));
			if (pOrder->VolumeTraded > 0)
			{
				calculate_position(code, direction, offset, static_cast<uint32_t>(pOrder->VolumeTraded), pOrder->LimitPrice, is_today);
			}
			this->fire_event(trader_event_type::TET_OrderPlace, entrust);
			if(offset == offset_type::OT_OPEN)
			{
				//开仓冻结资金
				query_account(false);
			}
			else
			{
				//平仓冻结仓位
				frozen_deduction(code, direction, entrust.total_volume, is_today);
			}
		}
		else
		{
			auto entrust = it->second;
			if(entrust.last_volume > static_cast<uint32_t>(pOrder->VolumeTotal))
			{
				calculate_position(code, direction, offset, entrust.last_volume - static_cast<uint32_t>(pOrder->VolumeTotal), pOrder->LimitPrice, is_today);
				entrust.last_volume = pOrder->VolumeTotal;
			}
			else
			{
				if (entrust.last_volume < static_cast<uint32_t>(pOrder->VolumeTotal))
				{
					LOG_ERROR("OnRtnOrder Error %llu %s %d < %d\n", estid, code.get_id(), entrust.last_volume, pOrder->VolumeTotal);
				}
			}
			_order_info[estid] = entrust;
		}

		if (pOrder->OrderStatus == THOST_FTDC_OST_PartTradedQueueing || pOrder->OrderStatus == THOST_FTDC_OST_PartTradedNotQueueing)
		{
			//触发 deal 事件
			this->fire_event(trader_event_type::TET_OrderDeal, estid, (uint32_t)pOrder->VolumeTotal, (uint32_t)(pOrder->VolumeTraded + pOrder->VolumeTotal));
		}
	}
	print_position("OnRtnOrder");
}

void ctp_trader::OnRtnTrade(CThostFtdcTradeField *pTrade)
{
	if (!_is_inited)
	{
		return;
	}
	if(pTrade&& pTrade->OffsetFlag != THOST_FTDC_OF_Open)
	{
		//平仓计算盈亏
		query_account(false);
	}
}

void ctp_trader::OnErrRtnOrderInsert(CThostFtdcInputOrderField *pInputOrder, CThostFtdcRspInfoField *pRspInfo)
{
	if (!_is_inited)
	{
		return;
	}
	if (pRspInfo && pRspInfo->ErrorID != 0)
	{
		LOG_ERROR("OnErrRtnOrderInsert \tErrorID = [%d] ErrorMsg = [%s]\n", pRspInfo->ErrorID, pRspInfo->ErrorMsg);
		print_position("OnErrRtnOrderInsert");
	}
	if(pInputOrder && pRspInfo)
	{
		LOG_ERROR("OnErrRtnOrderInsert %s %d %s \n", pInputOrder->InstrumentID, pInputOrder->VolumeTotalOriginal, pRspInfo->ErrorMsg);
		estid_t estid = generate_estid(_front_id, _session_id, strtol(pInputOrder->OrderRef,NULL,10));
		auto it = _order_info.find(estid);
		if(it != _order_info.end())
		{
			_order_info.erase(it);
		}
		this->fire_event(trader_event_type::TET_OrderError, error_type::ET_PLACE_ORDER,estid, (uint32_t)pRspInfo->ErrorID);
	}
}
void ctp_trader::OnErrRtnOrderAction(CThostFtdcOrderActionField* pOrderAction, CThostFtdcRspInfoField* pRspInfo)
{
	if (!_is_inited)
	{
		return;
	}
	if (pRspInfo && pRspInfo->ErrorID != 0)
	{
		LOG_ERROR("OnErrRtnOrderAction \tErrorID = [%d] ErrorMsg = [%s]\n", pRspInfo->ErrorID, pRspInfo->ErrorMsg);
		print_position("OnErrRtnOrderAction");
	}
	if (pOrderAction && pRspInfo)
	{
		estid_t estid = generate_estid(pOrderAction->FrontID, pOrderAction->SessionID, strtol(pOrderAction->OrderRef, NULL, 10));
		
		this->fire_event(trader_event_type::TET_OrderError, error_type::ET_CANCEL_ORDER, estid, (uint32_t)pRspInfo->ErrorID);
	}
}

void ctp_trader::OnRtnInstrumentStatus(CThostFtdcInstrumentStatusField* pInstrumentStatus)
{
	
}

bool ctp_trader::do_auth()
{
	if (_td_api == nullptr)
	{
		return false;
	}
	CThostFtdcReqAuthenticateField req;
	memset(&req, 0, sizeof(req));
	strcpy(req.BrokerID, _broker_id.c_str());
	strcpy(req.UserID, _userid.c_str());
	//strcpy(req.UserProductInfo, m_strProdInfo.c_str());
	strcpy(req.AuthCode, _authcode.c_str());
	strcpy(req.AppID, _appid.c_str());
	int iResult = _td_api->ReqAuthenticate(&req, genreqid());
	if (iResult != 0)
	{
		LOG_ERROR("ctp_trader do_auth request failed: %d", iResult);
		return false;
	}
	return true ;
}

bool ctp_trader::is_usable()const
{
	if (_td_api == nullptr)
	{
		return false;
	}
	if(!_is_inited)
	{
		return false ;
	}
	return _is_connected;
}


por_t ctp_trader::place_order(offset_type offset, direction_type direction, const code_t& code, uint32_t volume, double_t price, order_flag flag)
{
	LOG_INFO("ctp_trader place_order %s %d",code.get_id(), volume);

	if (_td_api == nullptr)
	{
		return INVALID_POR;
	}
	if (offset == offset_type::OT_CLOSE)
	{
		auto pos = _history_position.find(code);
		if (pos != _history_position.end())
		{
			auto posval = pos->second.get_volume(direction);
			if (posval>0)
			{
				if (posval < volume)
				{
					//需要拆单
					estid_t history_estid = do_order(offset, direction, code, posval, price, flag, true);
					estid_t today_estid = do_order(offset, direction, code, volume - posval, price, flag, false);
					return { today_estid, history_estid };
				}
				else
				{
					estid_t history_estid = do_order(offset, direction, code, posval, price, flag, true);
					return { INVALID_ESTID, history_estid };
				}
			}
		}
	}
	estid_t today_estid = do_order(offset, direction, code, volume, price, flag ,false);
	return { today_estid, INVALID_ESTID };
}

void ctp_trader::cancel_order(estid_t order_id)
{
	if (_td_api == nullptr)
	{
		LOG_ERROR("ctp_trader cancel_order _td_api nullptr : %llu", order_id);
		return ;
	}
	auto it = _order_info.find(order_id);
	if (it == _order_info.end())
	{
		LOG_ERROR("ctp_trader cancel_order order invalid : %llu", order_id);
		return;
	}
	auto& order = it->second;
	uint32_t frontid = 0, sessionid = 0, orderref = 0;
	extract_estid(order_id, frontid, sessionid, orderref);
	CThostFtdcInputOrderActionField req;
	memset(&req, 0, sizeof(req));
	strcpy(req.BrokerID, _broker_id.c_str());
	strcpy(req.InvestorID, _userid.c_str());
	strcpy(req.UserID, _userid.c_str());
	///报单引用
	sprintf(req.OrderRef, "%u", orderref);
	///请求编号
	///前置编号
	req.FrontID = frontid;
	///会话编号
	req.SessionID = sessionid;
	///操作标志
	req.ActionFlag = wrap_action_flag(action_flag::AF_CANCEL);
	///合约代码
	strcpy(req.InstrumentID, order.code.get_id());

	//req.LimitPrice = change.price;

	//req.VolumeChange = (int)change.volume;

	//strcpy_s(req.OrderSysID, change.order_id.c_str());
	strcpy(req.ExchangeID, order.code.get_excg());

	int iResult = _td_api->ReqOrderAction(&req, genreqid());
	if (iResult != 0)
	{
		LOG_ERROR("ctp_trader order_action request failed: %d", iResult);
	}
}

void ctp_trader::submit_settlement()
{
	if (_td_api == nullptr)
	{
		return;
	}

	//std::this_thread::sleep_for(std::chrono::milliseconds(1000));
	CThostFtdcSettlementInfoConfirmField req;
	memset(&req, 0, sizeof(req));
	strcpy(req.BrokerID, _broker_id.c_str());
	strcpy(req.InvestorID, _userid.c_str());

	//fmt::format_to(req.ConfirmDate, "{}", TimeUtils::getCurDate());
	//memcpy(req.ConfirmTime, TimeUtils::getLocalTime().c_str(), 8);

	int iResult = _td_api->ReqSettlementInfoConfirm(&req, genreqid());
	if (iResult != 0)
	{
		LOG_ERROR("ctp_trader submit_settlement request failed: %d", iResult);
	}
}
uint32_t ctp_trader::get_trading_day()const
{
	if(_td_api)
	{
		return static_cast<uint32_t>(std::atoi(_td_api->GetTradingDay()));
	}
	return 0X0U;
}

std::shared_ptr<trader_data> ctp_trader::get_trader_data()const
{
	auto result = std::make_shared<trader_data>();
	result->account = _account_info;
	for (auto it : _order_info)
	{
		result->orders.emplace_back(it.second);
	}
	for (auto it : _today_position)
	{
		result->positions[it.first]=it.second;
	}
	for (auto it : _history_position)
	{
		auto& current = result->positions[it.first];
		current.long_cell.volume = it.second.long_cell.volume;
		current.long_cell.frozen = it.second.long_cell.frozen;
		current.short_cell.volume = it.second.short_cell.volume;
		current.short_cell.frozen = it.second.short_cell.frozen;
	}
	return result ;
}

void ctp_trader::calculate_position(const code_t& code,direction_type dir_type, offset_type offset_type,uint32_t volume,double_t price,bool is_today)
{
	LOG_INFO("calculate_position %s %d %d %d %f %d", code.get_id(), dir_type, offset_type, volume, price, is_today);
	
	if (offset_type == offset_type::OT_OPEN)
	{
		position_info& p = _today_position[code];
		p.id = code;
		if (dir_type == direction_type::DT_LONG)
		{
			p.long_cell.volume += volume;
		}
		else
		{
			p.short_cell.volume += volume;
		}
	}
	else
	{
		if (is_today)
		{
			position_info& p = _today_position[code];
			p.id = code;
			if (dir_type == direction_type::DT_LONG)
			{
				if (p.long_cell.volume > volume)
				{
					p.long_cell.volume -= volume;
				}
				else
				{
					p.long_cell.volume = 0;
				}
				if (p.long_cell.frozen > volume)
				{
					p.long_cell.frozen -= volume;
				}
				else
				{
					p.long_cell.frozen = 0;
				}
			}
			else if (dir_type == direction_type::DT_SHORT)
			{
				if (p.short_cell.volume > volume)
				{
					p.short_cell.volume -= volume;
				}
				else
				{
					p.short_cell.volume = 0;
				}
				if (p.short_cell.frozen > volume)
				{
					p.short_cell.frozen -= volume;
				}
				else
				{
					p.short_cell.frozen = 0;
				}
			}
			if (p.empty())
			{
				auto it = _today_position.find(code);
				if (it != _today_position.end())
				{
					_today_position.erase(it);

				}
			}
		}
		else
		{
			position_info& p = _history_position[code];
			p.id = code;
			if (dir_type == direction_type::DT_LONG)
			{
				if (p.long_cell.volume > volume)
				{
					p.long_cell.volume -= volume;
				}
				else
				{
					p.long_cell.volume = 0;
				}
				if (p.long_cell.frozen > volume)
				{
					p.long_cell.frozen -= volume;
				}
				else
				{
					p.long_cell.frozen = 0;
				}
			}
			else if (dir_type == direction_type::DT_SHORT)
			{
				if (p.short_cell.volume > volume)
				{
					p.short_cell.volume -= volume;
				}
				else
				{
					p.short_cell.volume = 0;
				}
				if (p.short_cell.frozen > volume)
				{
					p.short_cell.frozen -= volume;
				}
				else
				{
					p.short_cell.frozen = 0;
				}
			}
			if (p.empty())
			{
				auto it = _history_position.find(code);
				if (it != _history_position.end())
				{
					_history_position.erase(it);

				}
			}
		}
	}

	print_position("calculate_position");
	this->fire_event(trader_event_type::TET_PositionChange, get_merge_position(code));
}

void ctp_trader::frozen_deduction(const code_t& code, direction_type dir_type, uint32_t volume,bool is_today)
{

	if(is_today)
	{
		auto it = _today_position.find(code);
		if (it == _today_position.end())
		{
			return;
		}
		position_info& pos = it->second;
		if (dir_type == direction_type::DT_LONG)
		{
			pos.long_cell.frozen += volume;
		}
		else if (dir_type == direction_type::DT_SHORT)
		{
			pos.short_cell.frozen += volume;
		}
	}
	else
	{
		auto it = _history_position.find(code);
		if (it == _history_position.end())
		{
			return;
		}
		position_info& pos = it->second;
		if (dir_type == direction_type::DT_LONG)
		{
			pos.long_cell.frozen += volume;
		}
		else if (dir_type == direction_type::DT_SHORT)
		{
			pos.short_cell.frozen += volume;
		}
	}
	print_position("frozen_deduction");
	this->fire_event(trader_event_type::TET_PositionChange, get_merge_position(code));

}
void ctp_trader::thawing_deduction(const code_t& code, direction_type dir_type, uint32_t volume,bool is_today)
{

	if(is_today)
	{
		auto it = _today_position.find(code);
		if (it == _today_position.end())
		{
			return;
		}
		position_info& pos = it->second;
		if (dir_type == direction_type::DT_LONG)
		{
			if(pos.long_cell.frozen > volume)
			{
				pos.long_cell.frozen -= volume;
			}else
			{
				pos.long_cell.frozen = 0;
			}
		}
		else if (dir_type == direction_type::DT_SHORT)
		{
			if (pos.short_cell.frozen > volume)
			{
				pos.short_cell.frozen -= volume;
			}
			else
			{
				pos.short_cell.frozen = 0;
			}
		}
	}
	else
	{
		auto it = _history_position.find(code);
		if (it == _history_position.end())
		{
			return;
		}
		position_info& pos = it->second;
		if (dir_type == direction_type::DT_LONG)
		{
			if (pos.long_cell.frozen > volume)
			{
				pos.long_cell.frozen -= volume;
			}
			else
			{
				pos.long_cell.frozen = 0;
			}
		}
		else if (dir_type == direction_type::DT_SHORT)
		{
			if (pos.short_cell.frozen > volume)
			{
				pos.short_cell.frozen -= volume;
			}
			else
			{
				pos.short_cell.frozen = 0;
			}
		}
	}
	print_position("thawing_deduction");
	this->fire_event(trader_event_type::TET_PositionChange, get_merge_position(code));
}