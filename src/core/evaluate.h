﻿#pragma once
#include "context.h"
#include "csv_recorder.h"

class evaluate : public context
{

private:
	
	std::shared_ptr<csv_recorder> _recorder;

	class dummy_market* _market_simulator;

	class dummy_trader* _trader_simulator;

public:
	evaluate();
	virtual ~evaluate();
public:

	bool init_from_file(const std::string& config_path);

	void playback_history(uint32_t tradeing_day);

	virtual trader_api& get_trader() override;

	virtual market_api& get_market() override;

	virtual void on_update() override;

	virtual bool is_terminaled() override;

	virtual void add_market_handle(std::function<void(market_event_type, const std::vector<std::any>&)> handle) override;

	virtual void add_trader_handle(std::function<void(trader_event_type, const std::vector<std::any>&)> handle) override;

};