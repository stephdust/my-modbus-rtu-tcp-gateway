# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Modbus RTU-TCP Gateway contributors.
import esphome.codegen as cg
from esphome.core import CORE
import esphome.config_validation as cv
from esphome.const import CONF_ID

gateway_ns = cg.esphome_ns.namespace("modbus_gateway")
ModbusGateway = gateway_ns.class_("ModbusGateway", cg.Component)

CONF_TCP_HOST = "tcp_host"
CONF_TCP_MODE = "tcp_mode"
CONF_TCP_PORT = "tcp_port"
CONF_BAUD = "baud"
CONF_DATA_BITS = "data_bits"
CONF_PARITY = "parity"
CONF_STOP_BITS = "stop_bits"
CONF_TCP_TIMEOUT = "tcp_timeout"

CONFIG_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.declare_id(ModbusGateway),
    cv.Optional(CONF_TCP_HOST, default="192.168.1.100"): cv.string,
    cv.Optional(CONF_TCP_MODE, default="CLIENT"): cv.one_of("CLIENT", "SERVER", upper=True),
    cv.Optional(CONF_TCP_PORT, default=502): cv.port,
    cv.Optional(CONF_BAUD, default=9600): cv.int_range(min=1200, max=115200),
    cv.Optional(CONF_DATA_BITS, default=8): cv.one_of(7, 8, int=True),
    cv.Optional(CONF_PARITY, default="NONE"): cv.one_of("NONE", "EVEN", "ODD", upper=True),
    cv.Optional(CONF_STOP_BITS, default=1): cv.one_of(1, 2, int=True),
    cv.Optional(CONF_TCP_TIMEOUT, default="3s"): cv.positive_time_period_milliseconds,
}).extend(cv.COMPONENT_SCHEMA)

async def to_code(config):
    # ESPHome 2026.2+ selective Arduino compilation: explicitly enable
    # the Arduino libraries used by modbus_gateway.h.
    if CORE.is_esp32 and CORE.using_arduino:
        cg.add_library("WiFi", None)
        cg.add_library("WebServer", None)
        cg.add_library("Preferences", None)

    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_tcp_host(config[CONF_TCP_HOST]))
    cg.add(var.set_tcp_mode(config[CONF_TCP_MODE]))
    cg.add(var.set_tcp_port(config[CONF_TCP_PORT]))
    cg.add(var.set_baud(config[CONF_BAUD]))
    cg.add(var.set_data_bits(config[CONF_DATA_BITS]))
    cg.add(var.set_parity(config[CONF_PARITY]))
    cg.add(var.set_stop_bits(config[CONF_STOP_BITS]))
    cg.add(var.set_tx_gpio(17))
    cg.add(var.set_rx_gpio(18))
    cg.add(var.set_de_re_gpio(21))
    cg.add(var.set_tcp_timeout(config[CONF_TCP_TIMEOUT].total_milliseconds))
