/* Local visual preview only: no radio, FC, firmware or network provisioning.
 * node tests/preview_web.cjs
 * node tests/preview_web.cjs --capture C:\absolute\workspace\preview.png
 */
'use strict';
const fs = require('node:fs');
const path = require('node:path');
const http = require('node:http');
const {spawn} = require('node:child_process');
const page = fs.readFileSync(path.join(__dirname,'../main/web/index.html'));
const started = Date.now();
let terminalPrint = true;
const reading = fields => ({age_s:0.5,stale:false,...fields});
function snapshot() {
  return {
    station:{uptime_ms:145000+Date.now()-started,radio_ready:true,key_configured:true,protocol_version:2,cipher:'AES-256-GCM',nonce_ready:true,tx_nonces_used:409,storage_ready:true,uplink_state:'connected',session:'40e326a81c94b752',peer_session:'74e219b331c6d820',last_sequence:411,last_rx_age_ms:800,rssi_dbm:-91,snr_db:6.5,rx_valid:411,rx_duplicates:2,rx_old:0,rx_missing:3,rx_invalid:0,rx_auth_errors:0,rx_format_errors:0,rx_crc_errors:1,rx_decode_errors:0,tx_count:409,tx_timeouts:0,ack_skipped:2,spi_errors:0,usb_dropped:0,frequency_hz:433775000,bandwidth_hz:125000,spreading_factor:7,coding_rate:5,sync_word:18,preamble:8,tx_power_dbm:5,duty_cycle:0.1,expected_period_ms:5000,usb_connected:true,web_ready:true,led_ready:true,led_gpio:21,led_state:'connected',request_history:[{id:17,opcode:2,status:'done',result:0}]},
    device:{uptime_ms:145000+Date.now()-started,terminal_print_enabled:terminalPrint,web_ready:true,web_ip:'192.168.4.1',ap_ssid:'VFPR-Station',ap_clients:1,free_heap_bytes:145216,minimum_free_heap_bytes:131440,led_gpio:21,led_enabled:true,web_enabled:true},
    telemetry:{target:{system:1,component:1},mode:'STABILIZE',armed:false,link:{status:'connected',heartbeat_age_s:0.8},_radio_period_s:5,_air_ack_age_s:1.2,_air_rtt_ms:114,_air_link_status:'connected',elapsed_s:203,received_messages:2893,bad_data_messages:0,
      attitude:reading({roll_deg:-0.4,pitch_deg:-0.8,yaw_deg:-159.9,roll_rate_deg_s:0,pitch_rate_deg_s:0,yaw_rate_deg_s:0}),position:reading({latitude_deg:0,longitude_deg:0,altitude_msl_m:0,relative_altitude_m:0.8,north_m_s:0,east_m_s:0,down_m_s:0,heading_deg:200.1}),gps:reading({fix_type:1,fix_label:'no fix',satellites:0,hdop:99.99}),battery:reading({voltage_v:11.58,current_a:0.32,remaining_pct:99,load_pct:19.4}),motion:reading({airspeed_m_s:0,groundspeed_m_s:0,heading_deg:200,throttle_pct:0,climb_m_s:0,altitude_msl_m:0}),rc:reading({channel_count:16,rssi_raw:null,channels:[1500,1500,999,1500,999,999,999,999,999,999,1443,999,2159,999,2011,2011,null,null]}),
      imus:[reading({id:0,acceleration_m_s2:[-0.19,0.23,-9.82],gyro_rad_s:[0,0,0],magnetic_field_mgauss:[-11,28,390],temperature_c:40}),reading({id:1,acceleration_m_s2:[-0.20,0.14,-9.80],gyro_rad_s:[0.001,0,0],magnetic_field_mgauss:[null,null,null],temperature_c:40.9})],pressures:[reading({id:0,absolute_hpa:997.2,differential_hpa:0,temperature_c:16.7})],vibration:reading({x_m_s2:0.01,y_m_s2:0.01,z_m_s2:0.01,clipping:[0,0,0]}),distance:null,optical_flow:null,
      outputs:reading({port:0,channels:[1000,1000,1000,1000,null,null,null,null,null,null,null,null,null,null,null,null]}),esc:[],health:reading({present:1409940527,enabled:1382128687,healthy:1409940527,unhealthy_mask:0,unhealthy:[]}),ekf:reading({flags:167,velocity_variance:0,horizontal_variance:0,vertical_variance:0,compass_variance:0,terrain_variance:null}),power:reading({vcc_v:null,servo_v:null,flags:0}),memory:reading({free_bytes:646758}),mcu:reading({id:0,temperature_c:49.3,voltage_v:3.3,voltage_min_v:3.3,voltage_max_v:3.3}),local_position:null,navigation:reading({roll_deg:0,pitch_deg:0,bearing_deg:-119,target_bearing_deg:0,waypoint_distance_m:0,altitude_error_m:0,airspeed_error_m_s:0,cross_track_error_m:0}),mission:reading({sequence:0,total:null}),home:null,batteries:[reading({id:0,voltage_v:null,current_a:0.32,remaining_pct:99,consumed_mah:35,energy_wh:0.39,temperature_c:null,cell_voltages_v:Array(14).fill(null)})],
      status_text:[{event_id:1,severity:2,age_s:128,text:'PreArm: GPS 1: Bad fix'},{event_id:2,severity:6,age_s:197,text:'GPS 1: u-blox protocol: 34.10'}],cache:{accepted_frames:411,invalid_frames:0,session_age_s:145,receive_age_s:0.8},last_result:{request_id:17,opcode:2,status:0,age_s:12}}
  };
}
const server = http.createServer((req,res)=>{
  res.setHeader('Cache-Control','no-store');
  if (req.url==='/api/telemetry' && req.method==='GET') {
    res.setHeader('Content-Type','application/json'); res.end(JSON.stringify(snapshot()));
  } else if (req.url==='/api/terminal' && req.method==='POST') {
    let input=''; req.on('data',chunk=>{input+=chunk;if(input.length>64)req.destroy();});
    req.on('end',()=>{
      try { const body=JSON.parse(input); if(typeof body.enabled!=='boolean')throw Error();terminalPrint=body.enabled;res.setHeader('Content-Type','application/json');res.end(JSON.stringify({terminal_print_enabled:terminalPrint})); }
      catch {res.writeHead(400);res.end();}
    });
  } else if (req.url==='/' && req.method==='GET') {
    res.setHeader('Content-Type','text/html; charset=utf-8');res.end(page);
  } else {res.writeHead(404);res.end();}
});
server.listen(8123,'127.0.0.1',()=>{
  console.log('Demo telemetry only: http://127.0.0.1:8123');
  if (process.argv[2]!=='--capture') return;
  const output=path.resolve(process.argv[3]||path.join(__dirname,'../../h743-web-preview.png'));
  const profile=path.resolve(__dirname,`../../h743-web-preview-profile-${process.pid}`);
  const edge='C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe';
  const child=spawn(edge,['--headless=new','--disable-gpu','--no-first-run','--hide-scrollbars',`--user-data-dir=${profile}`,'--window-size=1440,1200','--virtual-time-budget=3000',`--screenshot=${output}`,'http://127.0.0.1:8123'],{windowsHide:true,stdio:['ignore','ignore','pipe']});
  let diagnostics='';child.stderr.on('data',chunk=>{diagnostics+=chunk.toString();});
  const timeout=setTimeout(()=>{child.kill();},20000);
  child.on('error',error=>{clearTimeout(timeout);server.close();console.error(error.message);process.exitCode=1;});
  child.on('exit',code=>{
    clearTimeout(timeout);server.close();
    if(code===0&&fs.existsSync(output)) console.log(`Screenshot: ${output}`);
    else {console.error(`Edge capture failed (${code}): ${diagnostics.slice(-2000)}`);process.exitCode=1;}
  });
});
