//! The sections, one file each; they run in parallel.

use crate::harness::T;

mod market;
pub mod ota;
mod s01_apps;
mod s02_error_screen;
mod s03_protocol;
mod s04_notice;
mod s06_wifi;
mod s07_net_mqtt;
mod s08_mqtt_offline;
mod s09_web;
mod s10_oauth;
mod s11_rtc;
mod s15_cleanup;
mod s16_crash_loop;
mod s17_menu_nav;
mod s17_menu_uninstall;
mod s18_app_secrets;
mod s20_models;
mod s21_hardening;
mod s22_i2c;
mod s23_ble;
mod s24_remote_touch;
mod s25_ota_rollback;
mod s25_ota_usb;
mod s25_ota_wifi;
mod s26_sound;
mod s32_update_check;
mod s32_update_install;
mod s32_update_menu;
mod s33_sleep;
mod s34_power_off;
mod s35_power_usb;
mod s36_uart;
mod s37_pwm;
mod s38_sensors;
mod s39_sd;
mod s41_png;
mod s43_app_stage;
mod s44_app_updates;
mod s45_app_updates_menu;
mod s46_marketplace;
mod s47_shared_rules;
mod s48_secrets_push;
mod s49_sim_wifi;
mod s50_lua_api;
pub mod update;

pub type Section = fn(&mut T);

pub const ALL: &[(&str, Section)] = &[
    ("01-apps", s01_apps::run),
    ("02-error-screen", s02_error_screen::run),
    ("03-protocol", s03_protocol::run),
    ("04-notice", s04_notice::run),
    ("06-wifi", s06_wifi::run),
    ("07-net-mqtt", s07_net_mqtt::run),
    ("08-mqtt-offline", s08_mqtt_offline::run),
    ("09-web", s09_web::run),
    ("10-oauth", s10_oauth::run),
    ("11-rtc", s11_rtc::run),
    ("15-cleanup", s15_cleanup::run),
    ("16-crash-loop", s16_crash_loop::run),
    ("17-menu-nav", s17_menu_nav::run),
    ("17-menu-uninstall", s17_menu_uninstall::run),
    ("18-app-secrets", s18_app_secrets::run),
    ("20-models", s20_models::run),
    ("21-hardening", s21_hardening::run),
    ("22-i2c", s22_i2c::run),
    ("23-ble", s23_ble::run),
    ("24-remote-touch", s24_remote_touch::run),
    ("25-ota-rollback", s25_ota_rollback::run),
    ("25-ota-usb", s25_ota_usb::run),
    ("25-ota-wifi", s25_ota_wifi::run),
    ("26-sound", s26_sound::run),
    ("32-update-check", s32_update_check::run),
    ("32-update-install", s32_update_install::run),
    ("32-update-menu", s32_update_menu::run),
    ("33-sleep", s33_sleep::run),
    ("34-power-off", s34_power_off::run),
    ("35-power-usb", s35_power_usb::run),
    ("36-uart", s36_uart::run),
    ("37-pwm", s37_pwm::run),
    ("38-sensors", s38_sensors::run),
    ("39-sd", s39_sd::run),
    ("41-png", s41_png::run),
    ("43-app-stage", s43_app_stage::run),
    ("44-app-updates", s44_app_updates::run),
    ("45-app-updates-menu", s45_app_updates_menu::run),
    ("46-marketplace", s46_marketplace::run),
    ("47-shared-rules", s47_shared_rules::run),
    ("48-secrets-push", s48_secrets_push::run),
    ("49-sim-wifi", s49_sim_wifi::run),
    ("50-lua-api", s50_lua_api::run),
];
