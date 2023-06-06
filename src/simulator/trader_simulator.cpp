﻿#include "trader_simulator.h"
#include <data_types.hpp>
#include <event_center.hpp>
#include "contract_parser.h"
#include "./tick_loader/csv_tick_loader.h"
#include <boost/date_time/posix_time/posix_time.hpp>

bool trader_simulator::init(const boost::property_tree::ptree& config)
{
	try
	{
		_account_info.money = config.get<double_t>("initial_capital");
		auto contract_file = config.get<std::string>("contract_config");
		_contract_parser.init(contract_file);
		_interval = config.get<uint32_t>("interval",1);
	}
	catch (...)
	{
		LOG_ERROR("tick_simulator init error ");
		return false;
	}
	return true;
}

void trader_simulator::push_tick(const tick_info& tick)
{
	_current_time = tick.time;
	handle_submit();
	match_entrust(&tick);
	_last_frame_volume[tick.id] = tick.volume;
}
void trader_simulator::crossday(uint32_t trading_day)
{
	std::vector<order_info> order;
	_order_info.get_all_order(order);
	for (auto it : order)
	{
		std::vector<order_match> match;
		_order_info.get_order_match(match, it.code);
		for (const order_match& mit : match)
		{
			if(mit.state != OS_INVALID)
			{
				order_cancel(it, mit.is_today);
			}
		}
	}
	std::vector<order_info> residue_order;
	_order_info.get_all_order(residue_order);
	if(residue_order.size()>0)
	{
		_order_info.clear();
	}
	_position_info.crossday_move_position([this](const code_t& code)->void{
		this->fire_event(trader_event_type::TET_PositionChange, _position_info.get_merge_position(code));
	});
	_current_trading_day = trading_day;
}


uint32_t trader_simulator::get_trading_day()const
{
	return _current_trading_day;
}

bool trader_simulator::is_usable()const
{
	return true ;
}

por_t trader_simulator::place_order(offset_type offset, direction_type direction, const code_t& code, uint32_t count, double_t price, order_flag flag)
{

	bool is_today = true ;
	if (offset == offset_type::OT_CLOSE)
	{
		uint32_t usable = _position_info.get_history_usable(code, direction);
		if (usable > 0) 
		{
			if (count < usable)
			{
				//平昨
				order_info history_order;
				history_order.est_id = make_estid();
				LOG_DEBUG("tick_simulator::place_order history_order %llu \n", history_order.est_id);
				history_order.code = code;
				history_order.create_time = _current_time;
				history_order.offset = offset;
				history_order.direction = direction;
				history_order.total_volume = count;
				history_order.last_volume = count;
				history_order.price = price;
				_order_info.add_order(history_order, flag, false);
				return { INVALID_ESTID,history_order.est_id };
			}
			else
			{
				//平昨加平今
				order_info history_order;
				history_order.est_id = make_estid();
				LOG_DEBUG("tick_simulator::place_order history_order %llu \n", history_order.est_id);
				history_order.code = code;
				history_order.create_time = _current_time;
				history_order.offset = offset;
				history_order.direction = direction;
				history_order.total_volume = usable;
				history_order.last_volume = usable;
				history_order.price = price;
				_order_info.add_order(history_order, flag, false);

				order_info today_order;
				today_order.est_id = make_estid();
				LOG_DEBUG("tick_simulator::place_order today_order %llu \n", today_order.est_id);
				today_order.code = code;
				today_order.create_time = _current_time;
				today_order.offset = offset;
				today_order.direction = direction;
				today_order.total_volume = usable - count;
				today_order.last_volume = usable - count;
				today_order.price = price;
				_order_info.add_order(today_order, flag, true);
				return { today_order.est_id,history_order.est_id };
			}
		}
		else 
		{
			order_info today_order;
			today_order.est_id = make_estid();
			LOG_DEBUG("tick_simulator::place_order today_order %llu \n", today_order.est_id);
			today_order.code = code;
			today_order.create_time = _current_time;
			today_order.offset = offset;
			today_order.direction = direction;
			today_order.total_volume = usable - count;
			today_order.last_volume = usable - count;
			today_order.price = price;
			_order_info.add_order(today_order, flag, true);
			return { today_order.est_id,INVALID_ESTID };
		}
	}
	else 
	{
		order_info order;
		order.est_id = make_estid();
		LOG_DEBUG("tick_simulator::place_order %llu \n", order.est_id);
		order.code = code;
		order.create_time = _current_time;
		order.offset = offset;
		order.direction = direction;
		order.total_volume = count;
		order.last_volume = count;
		order.price = price;
		_order_info.add_order(order, flag, true);
		return { order.est_id,INVALID_ESTID };
	}
	
}

void trader_simulator::cancel_order(estid_t order_id)
{
	LOG_DEBUG("tick_simulator cancel_order %lld", order_id);
	auto match = _order_info.get_order_match(order_id);
	if(match)
	{
		if(match->state == OS_INVALID)
		{
			return;
		}
		if(match->state != OS_CANELED)
		{
			_order_info.set_state(order_id, OS_CANELED);
		}
	}
}

void trader_simulator::submit_settlement()
{
	while (!_is_submit_return.exchange(false));
}

std::shared_ptr<trader_data> trader_simulator::get_trader_data()const
{
	auto result = std::make_shared<trader_data>();
	result->account = _account_info;
	_order_info.get_valid_order(result->orders);
	_position_info.get_all_position(result->positions);
	return result;
}

void trader_simulator::handle_submit()
{
	if (!_is_submit_return)
	{
		while(!_is_submit_return.exchange(true));
		this->fire_event(trader_event_type::TET_SettlementCompleted);
	}
}


estid_t trader_simulator::make_estid()
{
	_order_ref++;
	uint64_t p1 = (uint64_t)_current_time<<32;
	uint64_t p2 = (uint64_t)0<<16;
	uint64_t p3 = (uint64_t)_order_ref;

	uint64_t v1 = p1 & 0xFFFFFFFF00000000LLU;
	uint64_t v2 = p2 & 0x00000000FFFF0000LLU;
	uint64_t v3 = p3 & 0x000000000000FFFFLLU;
	return v1 + v2 + v3;
}

uint32_t trader_simulator::get_front_count(const code_t& code,double_t price)
{
	auto tick_it = std::find_if(_current_tick_info.begin(), _current_tick_info.end(),[code](auto cur) ->bool {
		if(cur->id == code)
		{
			return true ;
		}
		return false ;
	});
	if(tick_it != _current_tick_info.end())
	{
		const auto& tick = *tick_it;
		auto buy_it = std::find_if(tick->buy_order.begin(), tick->buy_order.end(), [price](const std::pair<double_t,uint32_t>& cur) ->bool {
			
			return cur.first == price;
			});
		if(buy_it != tick->buy_order.end())
		{
			return buy_it->second ;
		}
		auto sell_it = std::find_if(tick->sell_order.begin(), tick->sell_order.end(), [price](const std::pair<double_t, uint32_t>& cur) ->bool {
			
			return cur.first == price;
			});
		if (sell_it != tick->sell_order.end())
		{
			return sell_it->second;
		}
	}

	return 0;
}

void trader_simulator::match_entrust(const tick_info* tick)
{
	auto last_volume = _last_frame_volume.find(tick->id);
	if(last_volume == _last_frame_volume.end())
	{
		return ;
	}
	uint32_t current_volume = tick->volume - last_volume->second ;
	std::vector<order_match> match;
	_order_info.get_order_match(match,tick->id);
	for(const order_match & it : match)
	{
		handle_entrust(tick, it, current_volume);
	}
}

void trader_simulator::handle_entrust(const tick_info* tick, const order_match& match, uint32_t max_volume)
{
	order_info& order = match.order;
	if(match.state == OS_INVALID)
	{
		uint32_t err = frozen_deduction(order.est_id, order.code, order.offset, order.direction, order.last_volume, order.price, match.is_today);
		if (err == 0U)
		{
			this->fire_event(trader_event_type::TET_OrderPlace, order);
			auto queue_seat = get_front_count(order.code, order.price);
			_order_info.set_seat(order.est_id, queue_seat);
			_order_info.set_state(order.est_id, OS_IN_MATCH);
		}
		else
		{
			order_error(error_type::ET_PLACE_ORDER,order.est_id, err);
		}
		return ;
	}
	if(match.state == OS_CANELED)
	{
		//撤单
		order_cancel(order, match.is_today);
		return;
	}

	if (order.direction == direction_type::DT_LONG)
	{	
		if(order.offset == offset_type::OT_OPEN)
		{
			handle_buy(tick, match, max_volume);
		}
		else if (order.offset == offset_type::OT_CLOSE)
		{
			handle_sell(tick, match, max_volume);
		}
		
	}
	else if (order.direction == direction_type::DT_SHORT)
	{
		if (order.offset == offset_type::OT_CLOSE)
		{
			handle_buy(tick, match, max_volume);
		}
		else
		{
			handle_sell(tick, match, max_volume);
		}
	}
}
void trader_simulator::handle_sell(const tick_info* tick,const order_match& match, uint32_t max_volume)
{
	order_info& order = match.order;
	if (order.price == 0)
	{
		//市价单直接成交(暂时先不考虑一次成交不完的情况)
		order.price = _order_info.set_price(order.est_id,tick->buy_price());
	}
	if (match.flag == order_flag::OF_FOK)
	{
		if (order.last_volume <= max_volume && order.price <= tick->buy_price())
		{
			//全成
			order_deal(order, order.last_volume, match.is_today);
		}
		else
		{
			//全撤
			order_cancel(order, match.is_today);
		}
	}
	else if (match.flag == order_flag::OF_FAK)
	{
		if(order.price <= tick->buy_price())
		{
			//部成
			uint32_t deal_volume = order.last_volume > max_volume ? max_volume : order.last_volume;
			if (deal_volume > 0)
			{
				order_deal(order, deal_volume, match.is_today);
			}
			uint32_t cancel_volume = order.last_volume - max_volume;
			if (cancel_volume > 0)
			{
				//部撤
				order_cancel(order, match.is_today);
			}
		}
		else
		{
			order_cancel(order, match.is_today);
		}
	}
	else
	{
		if (order.price <= tick->buy_price())
		{
			//不需要排队，直接降价成交
			uint32_t deal_volume = order.last_volume > max_volume ? max_volume : order.last_volume;
			if (deal_volume > 0)
			{
				order_deal(order, deal_volume, match.is_today);
			}
		}
		else if (order.price <= tick->price)
		{
			//排队成交，移动排队位置
			int32_t new_seat = match.queue_seat - max_volume;
			if (new_seat < 0)
			{
				//排队到了，有成交了
				//deal_count = - new_seat
				_order_info.set_seat(order.est_id,0);
				uint32_t can_deal_volume = static_cast<uint32_t>(-new_seat);
				uint32_t deal_volume = order.last_volume > can_deal_volume ? can_deal_volume : order.last_volume;
				if (deal_volume > 0U)
				{
					order_deal(order, deal_volume, match.is_today);
				}
			}
			else
			{
				_order_info.set_seat(order.est_id, new_seat);
			}
		}
	}

	

}

void trader_simulator::handle_buy(const tick_info* tick, const order_match& match, uint32_t max_volume)
{
	order_info& order = match.order;
	if (order.price == 0)
	{
		//市价单直接成交
		order.price = _order_info.set_price(order.est_id,tick->sell_price());
	}
	if (match.flag == order_flag::OF_FOK)
	{
		if (order.last_volume <= max_volume&& order.price >= tick->sell_price())
		{
			//全成
			order_deal(order, order.last_volume, match.is_today);
		}
		else
		{
			//全撤
			order_cancel(order, match.is_today);
		}
	}
	else if (match.flag == order_flag::OF_FAK)
	{
		//部成
		if(order.price >= tick->sell_price())
		{
			uint32_t deal_volume = order.last_volume > max_volume ? max_volume : order.last_volume;
			if (deal_volume > 0U)
			{
				order_deal(order, deal_volume, match.is_today);
			}
			uint32_t cancel_volume = order.last_volume - max_volume;
			if (cancel_volume > 0)
			{
				//部撤
				order_cancel(order, match.is_today);
			}
		}
		else
		{
			order_cancel(order, match.is_today);
		}
		
	}
	else
	{
		//剩下都不是第一帧自动撤销的订单
		if (order.price >= tick->sell_price())
		{
			//不需要排队，直接降价成交
			uint32_t deal_volume = order.last_volume > max_volume ? max_volume : order.last_volume;
			if (deal_volume > 0)
			{
				order_deal(order, deal_volume, match.is_today);
			}
		}
		else if (order.price >= tick->price)
		{
			//有排队的情况
			//排队成交，移动排队位置
			int32_t new_seat = match.queue_seat - max_volume;
			if (new_seat < 0)
			{
				//排队到了，有成交了
				//deal_count = - new_seat
				_order_info.set_seat(order.est_id, 0);
				uint32_t can_deal_volume = static_cast<uint32_t>(-new_seat);
				uint32_t deal_volume = order.last_volume > can_deal_volume ? can_deal_volume : order.last_volume;
				if (deal_volume > 0)
				{
					order_deal(order, deal_volume, match.is_today);
				}
			}
			else
			{
				_order_info.set_seat(order.est_id, new_seat);
			}
		}
	}
}

void trader_simulator::order_deal(order_info& order, uint32_t deal_volume,bool is_today)
{
	
	auto contract_info = _contract_parser.get_contract_info(order.code);
	if(contract_info == nullptr)
	{
		LOG_ERROR("tick_simulator order_deal cant find the contract_info for %s", order.code.get_id());
		return;
	}
	double_t service_charge = contract_info->get_service_charge(order.price, order.offset, is_today);
	if(order.offset == offset_type::OT_OPEN)
	{
		//开仓
		if(order.direction == direction_type::DT_LONG)
		{
			
			if(_account_info.money >= deal_volume * service_charge)
			{
				_position_info.increase_position(order.code,order.direction, order.price, deal_volume);
				this->fire_event(trader_event_type::TET_PositionChange, _position_info.get_merge_position(order.code));
				_account_info.money -=  deal_volume * service_charge;
				this->fire_event(trader_event_type::TET_AccountChange, _account_info);
			}
			
		}
		else if (order.direction == direction_type::DT_SHORT)
		{
			if(_account_info.money >= deal_volume * service_charge)
			{
				_position_info.increase_position(order.code, order.direction, order.price, deal_volume);
				this->fire_event(trader_event_type::TET_PositionChange, _position_info.get_merge_position(order.code));
				_account_info.money -= (deal_volume * service_charge);
				this->fire_event(trader_event_type::TET_AccountChange, _account_info);
			}
		}
	}
	else
	{
		//平仓
		if (order.direction == direction_type::DT_LONG)
		{
			double_t long_price = _position_info.get_avg_price(order.code, order.direction);
			_account_info.money += (deal_volume * (order.price - long_price) * contract_info->multiple);
			_position_info.reduce_position(order.code, order.direction, deal_volume, is_today);
			_account_info.frozen_monery -= (deal_volume * long_price * contract_info->multiple * contract_info->margin_rate);
			this->fire_event(trader_event_type::TET_PositionChange, _position_info.get_merge_position(order.code));
			_account_info.money -= (deal_volume * service_charge);
			this->fire_event(trader_event_type::TET_AccountChange, _account_info);
		}
		else if (order.direction == direction_type::DT_SHORT)
		{
			double_t long_price = _position_info.get_avg_price(order.code, order.direction);
			_account_info.money += (deal_volume * (long_price - order.price) * contract_info->multiple);
			_position_info.reduce_position(order.code, order.direction, deal_volume, is_today);
			_account_info.frozen_monery -= (deal_volume * long_price * contract_info->multiple * contract_info->margin_rate);
			this->fire_event(trader_event_type::TET_PositionChange, _position_info.get_merge_position(order.code));
			_account_info.money -= deal_volume * service_charge;
			this->fire_event(trader_event_type::TET_AccountChange, _account_info);
		}
	}
	
	order.last_volume =_order_info.set_last_volume(order.est_id,order.last_volume - deal_volume);
	if(order.last_volume > 0)
	{
		//部分成交
		this->fire_event(trader_event_type::TET_OrderDeal, order.est_id, order.total_volume - order.last_volume, order.total_volume);
	}
	else
	{
		LOG_TRACE(" order_deal _order_info.del_order %llu", order.est_id);
		//全部成交
		this->fire_event(trader_event_type::TET_OrderTrade, order.est_id, order.code, order.offset, order.direction, order.price, order.total_volume);
		_order_info.del_order(order.est_id);
	}
	
}
void trader_simulator::order_error(error_type type,estid_t estid,uint32_t err)
{
	this->fire_event(trader_event_type::TET_OrderError, type, estid, err);
	if (_order_info.exist(estid))
	{
		LOG_TRACE(" order_error _order_info.del_order %llu", estid);
		_order_info.del_order(estid);
	}
}
void trader_simulator::order_cancel(const order_info& order,bool is_today)
{
	if(!_order_info.exist(order.est_id))
	{
		return;
	}
	if(order.last_volume>0)
	{
		if(thawing_deduction(order.code, order.offset, order.direction, order.last_volume, order.price, is_today))
		{
			LOG_INFO(" order_cancel _order_info.del_order %llu", order.est_id);
			this->fire_event(trader_event_type::TET_OrderCancel, order.est_id, order.code, order.offset, order.direction, order.price, order.last_volume, order.total_volume);
			_order_info.del_order(order.est_id);
		}
		else
		{
			LOG_ERROR("order_cancel error");
		}
	}
}
uint32_t trader_simulator::frozen_deduction(estid_t est_id,const code_t& code,offset_type offset, direction_type direction,uint32_t count,double_t price,bool is_today)
{
	auto contract_info = _contract_parser.get_contract_info(code);
	if (contract_info == nullptr)
	{
		LOG_ERROR("tick_simulator frozen_deduction cant find the contract_info for %s", code.get_id());
		return 10000U;
	}
	if (offset == offset_type::OT_OPEN)
	{
		double_t frozen_monery = (count * price * contract_info->multiple * contract_info->margin_rate);
		if (frozen_monery + _account_info.frozen_monery > _account_info.money)
		{
			return 31U;
		}
		//开仓 冻结保证金
		_account_info.frozen_monery += frozen_monery;
		this->fire_event(trader_event_type::TET_AccountChange, _account_info);
		return 0U ;
	}
	if (offset == offset_type::OT_CLOSE)
	{
		
		if (direction == direction_type::DT_LONG)
		{
			if(is_today)
			{
				const auto&& pos = _position_info.get_today_position(code);
				LOG_TRACE("frozen_deduction long today %s %llu %u", code.get_id(), est_id, pos.long_cell.usable());
				if (pos.long_cell.usable() < count)
				{
					return 30U;
				}
				_position_info.frozen_position(code, direction, count, is_today);
			}
			else
			{
				const auto&& pos = _position_info.get_history_position(code);
				LOG_TRACE("frozen_deduction long yestoday %s %llu %u", code.get_id(), est_id, pos.long_cell.usable());
				if (pos.long_cell.usable() < count)
				{
					return 30U;
				}
				_position_info.frozen_position(code, direction, count, is_today);
			}
			
		}
		else if (direction == direction_type::DT_SHORT)
		{
			if(is_today)
			{
				const auto&& pos = _position_info.get_today_position(code);
				LOG_TRACE("frozen_deduction short today %s %llu %u", code.get_id(), est_id, pos.short_cell.usable());
				if (pos.short_cell.usable() < count)
				{
					return 30U;
				}
				_position_info.frozen_position(code, direction, count, is_today);
			}
			else
			{
				const auto&& pos = _position_info.get_history_position(code);
				LOG_TRACE("frozen_deduction short yestoday %s %llu %u", code.get_id(), est_id, pos.short_cell.usable());
				if (pos.short_cell.usable() < count)
				{
					return 30U;
				}
				_position_info.frozen_position(code, direction, count, is_today);
			}
		}
		this->fire_event(trader_event_type::TET_PositionChange, _position_info.get_merge_position(code));
		return 0U;
	}
	return 23U;
}
bool trader_simulator::thawing_deduction(const code_t& code, offset_type offset, direction_type direction, uint32_t last_volume, double_t price,bool is_today)
{
	auto contract_info = _contract_parser.get_contract_info(code);
	if (contract_info == nullptr)
	{
		LOG_ERROR("tick_simulator frozen_deduction cant find the contract_info for %s", code.get_id());
		return false;
	}
	//double_t service_charge = contract_info->get_service_charge(price, offset, is_today);
	//_account_info.money+= last_volume * service_charge;
	if (offset == offset_type::OT_OPEN)
	{
		double_t delta = (last_volume * price * contract_info->multiple * contract_info->margin_rate);
		//撤单 取消冻结保证金
		LOG_TRACE("thawing_deduction 1 %f %f %u %f", _account_info.frozen_monery, delta, last_volume , price);
		_account_info.frozen_monery -= delta;
		LOG_TRACE("thawing_deduction 2 %f %f", _account_info.frozen_monery, delta);
		this->fire_event(trader_event_type::TET_AccountChange, _account_info);
	}
	else if (offset == offset_type::OT_CLOSE)
	{
		_position_info.thawing_position(code, direction, last_volume, is_today);
		this->fire_event(trader_event_type::TET_PositionChange, _position_info.get_merge_position(code));
	}
	
	return true;
}
