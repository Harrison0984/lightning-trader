﻿#pragma once
#include "define.h"


constexpr size_t CODE_DATA_LEN = 24;
#define EXCG_OFFSET_LEN	6
#define EXCG_BEGIN_INDEX	CODE_DATA_LEN-EXCG_OFFSET_LEN

#if defined(WIN32)
#pragma  warning(disable:4996)
#endif

struct code_t
{
private:
	char _data[CODE_DATA_LEN];

public:

	code_t(const char* cd)
	{
		size_t ed = 0;
		std::memset(&_data, 0, sizeof(_data));

		for (size_t i = 0; cd[i] != '\0' && i < CODE_DATA_LEN; i++)
		{
			char c = cd[i];
			if (c == '.')
			{
				ed = i;
				continue;
			}
			if (ed == 0)
			{
				_data[i + EXCG_BEGIN_INDEX] = cd[i];
			}
			else
			{
				_data[i - ed - 1] = cd[i];
			}
		}
	}

	code_t()
	{
		std::memset(&_data, 0, sizeof(_data));
	}

	code_t(const code_t& obj)
	{
		std::memcpy(_data, obj._data, CODE_DATA_LEN);
	}
	code_t(const char* id, const char* excg_id)
	{
		std::memset(&_data, 0, sizeof(_data));
		if(std::strlen(id) < EXCG_BEGIN_INDEX)
		{
			std::strcpy(_data, id);
		}
		if (std::strlen(excg_id) < EXCG_BEGIN_INDEX)
		{
			std::strcpy(_data + EXCG_BEGIN_INDEX,excg_id);
		}
	}

	bool operator < (const code_t& other)const
	{
		return std::memcmp(_data, other._data, CODE_DATA_LEN) < 0;
	}
	bool operator == (const code_t& other)const
	{
		return std::memcmp(_data, other._data, CODE_DATA_LEN) == 0;
	}
	bool operator != (const code_t& other)const
	{
		return std::memcmp(_data, other._data, CODE_DATA_LEN) != 0;
	}

	const char* get_id()const
	{
		return _data;
	}
	const char* get_excg()const
	{
		return _data + EXCG_BEGIN_INDEX;
	}

	inline bool is_split_position()const
	{
		if (std::strcmp(get_excg(), EXCHANGE_ID_SHFE) == 0)
		{
			return true;
		}
		return false;
	}
};

const code_t default_code;

struct tick_info
{
	code_t id; //合约ID

	time_t time; //时间

	uint32_t tick; //毫秒数

	double_t price;  //pDepthMarketData->LastPrice

	double_t open;
	
	double_t close;

	double_t high;

	double_t low;

	double_t standard;

	uint32_t volume;

	uint32_t trading_day;

	double_t open_interest;

	std::array<std::pair<double_t, uint32_t>, 5> buy_order;
	std::array<std::pair<double_t, uint32_t>, 5> sell_order;

	double_t buy_price()const
	{
		if (buy_order.empty())
		{
			return 0;
		}
		return buy_order[0].first;
	}
	double_t sell_price()const
	{
		if (sell_order.empty())
		{
			return 0;
		}
		return sell_order[0].first;
	}
	tick_info()
		:time(0),
		tick(0),
		open(0),
		close(0),
		high(0),
		low(0),
		price(0),
		standard(0),
		volume(0),
		trading_day(0),
		open_interest(.0F)
	{}


	int32_t total_buy_valume()const
	{
		int32_t reuslt = 0;
		for (auto& it : buy_order)
		{
			reuslt += it.second;
		}
		return reuslt;
	}

	int32_t total_sell_valume()const
	{
		int32_t reuslt = 0;
		for (auto& it : sell_order)
		{
			reuslt += it.second;
		}
		return reuslt;
	}

	bool invalid()const
	{
		return id==default_code;
	}
};

const tick_info default_tick_info;


/*
 *	订单标志
 */
enum class order_flag
{
	OF_NOR = '0',		//普通订单
	OF_FAK,			//全成全撤，不等待自动撤销
	OF_FOK,			//部成部撤，不等待自动撤销
};

/*
 *	开平方向
 */
enum class offset_type
{
	OT_OPEN = '0',	//开仓
	OT_CLOSE		//平仓,上期为平昨
};

/*
 *	多空方向
 */
enum class direction_type
{
	DT_LONG = '0',	//做多
	DT_SHORT		//做空
};

enum class deal_direction
{
	DD_DOWN = -1,	//向下
	DD_FLAT = 0,	//平
	DD_UP = 1,		//向上
};

enum class deal_status
{
	DS_INVALID,
	DS_DOUBLE_OPEN, //双开
	DS_OPEN,		//开仓
	DS_CHANGE,		//换手
	DS_CLOSE,		//平仓
	DS_DOUBLE_CLOSE,//双平

};

struct deal_info
{
	//现手
	uint32_t	volume_delta;
	//增仓
	double_t	interest_delta;
	//方向
	deal_direction	direction;

	deal_info() :
		volume_delta(0),
		interest_delta(.0F),
		direction(deal_direction::DD_FLAT)
	{}

	deal_status get_status()
	{
		if(volume_delta == interest_delta && interest_delta > 0)
		{
			return deal_status::DS_DOUBLE_OPEN;
		}
		if (volume_delta > interest_delta && interest_delta > 0)
		{
			return deal_status::DS_OPEN;
		}
		if (volume_delta > interest_delta && interest_delta == 0)
		{
			return deal_status::DS_CHANGE;
		}
		if (volume_delta > -interest_delta && -interest_delta > 0)
		{
			return deal_status::DS_CLOSE;
		}
		if (volume_delta == -interest_delta && -interest_delta > 0)
		{
			return deal_status::DS_DOUBLE_CLOSE;
		}
	}
};


struct bar_info
{
	code_t id; //合约ID

	time_t time; //时间

	double_t open;

	double_t close;

	double_t high;

	double_t low;

	//成交量
	uint32_t volume;

	//订单流中delta（price_buy_volume - price_sell_volume）
	int32_t delta ;

	//订单流中poc (最大成交量的价格，表示bar重心)
	double_t poc ;

	//订单流中的明细
	std::map<double_t, uint32_t> price_buy_volume;
	std::map<double_t, uint32_t> price_sell_volume;

	std::vector<double_t> buy_unbalance(uint32_t multiple)const
	{
		return std::vector<double_t>();
	}
	std::vector<double_t> sell_unbalance()const
	{
		
		return std::vector<double_t>();
	}

	void clear() 
	{
		time = 0;
		open = .0F;
		high = .0F;
		low = .0F;
		close = .0F;
		volume = 0;
		delta = 0;
		poc = 0;
		price_buy_volume.clear();
		price_sell_volume.clear();
	}

	bar_info()
		:time(0),
		open(0),
		close(0),
		high(0),
		low(0),
		volume(0),
		delta(0),
		poc(.0F)
	{}


};


struct position_cell
{
	//仓位
	uint32_t	volume;
	//冻结
	uint32_t	frozen;

	position_cell() :
		volume(0),
		frozen(0)
	{}

	uint32_t usable()const
	{
		return volume - frozen;
	}

	bool empty()const
	{
		return volume == 0;
	}

	void clear()
	{
		volume = 0;
		frozen = 0 ;
	}


};

struct position_info
{
	code_t id; //合约ID
	position_info(const code_t& code):id(code) {}
	//仓位
	position_cell long_cell;
	position_cell short_cell;


	bool empty()const
	{
		return long_cell.empty()&& short_cell.empty();
	}

	uint32_t get_total()const 
	{
		return long_cell.volume + short_cell.volume;
	}

	uint32_t get_volume(direction_type direction)const
	{
		if (direction == direction_type::DT_LONG)
		{
			return long_cell.volume;
		}
		else 
		{
			return short_cell.volume;
		}
	}

	uint32_t get_frozen(direction_type direction)const
	{
		if (direction == direction_type::DT_LONG)
		{
			return long_cell.frozen;
		}
		else
		{
			return short_cell.frozen;
		}
		
	}

	position_info()
	{}
};
const position_info default_position;

struct account_info
{
	double money;

	double frozen_monery;

	account_info() :
		money(.0F),
		frozen_monery(.0F)
		
	{}
};
const account_info default_account;

struct order_info
{
	
	estid_t		est_id;

	code_t			code;

	std::string		unit_id ;

	uint32_t		total_volume;

	uint32_t		last_volume;

	time_t			create_time;

	offset_type		offset;

	direction_type	direction;

	double_t		price;

	order_info() :
		est_id(INVALID_ESTID),
		offset(offset_type::OT_OPEN),
		direction(direction_type::DT_LONG),
		total_volume(0),
		last_volume(0),
		create_time(0),
		price(.0f)
	{}

	bool is_valid()
	{
		return est_id != INVALID_ESTID;
	}

	
};
const order_info default_order ;


//订单统计数据
struct order_statistic
{
	//下单数量
	uint32_t place_order_amount;
	//委托数量
	uint32_t entrust_amount;
	//成交数量
	uint32_t trade_amount;
	//撤单数量
	uint32_t cancel_amount;
	//错误数量
	uint32_t error_amount;

	order_statistic():
		place_order_amount(0),
		entrust_amount(0),
		trade_amount(0),
		cancel_amount(0),
		error_amount(0)
	{}

};

const order_statistic default_statistic;


enum class error_type
{
	ET_PLACE_ORDER,
	ET_CANCEL_ORDER,
	ET_OTHER_ERROR
};

struct today_market_info
{
	code_t code ;
	
	std::map<double_t, uint32_t> volume_distribution;

	std::vector<tick_info> today_tick_info;

	std::map<uint32_t, std::vector<bar_info>> today_bar_info;

	std::vector<bar_info>* get_history_bar(uint32_t period)
	{

		auto it = today_bar_info.find(period);
		if (it == today_bar_info.end())
		{
			return nullptr;
		}
		return &it->second;
	}

	void clear()
	{
		volume_distribution.clear();
		today_tick_info.clear();
		today_bar_info.clear();
	}
};
const today_market_info default_today_market_info;

