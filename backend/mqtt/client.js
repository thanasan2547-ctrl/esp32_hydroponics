/**
 * MQTT Client Module
 * Connects to HiveMQ Cloud and manages subscriptions/publishing
 */
const mqtt = require('mqtt');

// ---- Topic Definitions ----
const TOPICS = {
  // Sensor topics (ESP32 publishes, backend subscribes)
  SENSOR_EC:    'hydroponics/sensor/ec',
  SENSOR_PH:    'hydroponics/sensor/ph',
  SENSOR_WATER: 'hydroponics/sensor/water',

  // Pump state topics (ESP32 publishes current state)
  PUMP_A:    'hydroponics/pump/a',
  PUMP_B:    'hydroponics/pump/b',
  PUMP_PH:   'hydroponics/pump/ph',
  PUMP_MAIN: 'hydroponics/pump/main',

  // Control topics (backend/frontend publishes, ESP32 subscribes)
  CONTROL_EC_TARGET: 'hydroponics/control/ec_target',
  CONTROL_PH_TARGET: 'hydroponics/control/ph_target',

  // System topics
  SYSTEM_AUTO:      'hydroponics/system/auto',
  SYSTEM_EMERGENCY: 'hydroponics/system/emergency',
  SYSTEM_STATUS:    'hydroponics/system/status',
};

let client = null;
let latestData = {
  ec: null,
  ph: null,
  water: null,
  pumpA: false,
  pumpB: false,
  pumpPh: false,
  pumpMain: false,
  autoMode: false,
  lastUpdate: null,
};

/**
 * Initialize MQTT connection
 * @param {Function} onSensorData - callback(type, value) when sensor data arrives
 */
function connect(onSensorData) {
  const brokerUrl = process.env.MQTT_BROKER_URL;
  const options = {
    username: process.env.MQTT_USER,
    password: process.env.MQTT_PASS,
    clientId: 'backend-hydro-' + Math.random().toString(16).substr(2, 8),
    clean: true,
    connectTimeout: 10000,
    reconnectPeriod: 5000,
    rejectUnauthorized: false,
  };

  client = mqtt.connect(brokerUrl, options);

  client.on('connect', () => {
    console.log('[MQTT] Connected to HiveMQ Cloud');

    // Subscribe to all sensor + pump + system topics
    const subTopics = [
      TOPICS.SENSOR_EC,
      TOPICS.SENSOR_PH,
      TOPICS.SENSOR_WATER,
      TOPICS.PUMP_A,
      TOPICS.PUMP_B,
      TOPICS.PUMP_PH,
      TOPICS.PUMP_MAIN,
      TOPICS.SYSTEM_AUTO,
      TOPICS.SYSTEM_STATUS,
    ];

    client.subscribe(subTopics, { qos: 1 }, (err) => {
      if (err) console.error('[MQTT] Subscribe error:', err);
      else console.log('[MQTT] Subscribed to:', subTopics.join(', '));
    });
  });

  client.on('message', (topic, message) => {
    const payload = message.toString();

    switch (topic) {
      case TOPICS.SENSOR_EC:
        latestData.ec = parseFloat(payload);
        latestData.lastUpdate = new Date();
        if (onSensorData) onSensorData('ec', latestData.ec);
        break;
      case TOPICS.SENSOR_PH:
        latestData.ph = parseFloat(payload);
        latestData.lastUpdate = new Date();
        if (onSensorData) onSensorData('ph', latestData.ph);
        break;
      case TOPICS.SENSOR_WATER:
        latestData.water = parseFloat(payload);
        latestData.lastUpdate = new Date();
        if (onSensorData) onSensorData('water', latestData.water);
        break;
      case TOPICS.PUMP_A:
        latestData.pumpA = payload === '1' || payload === 'true';
        break;
      case TOPICS.PUMP_B:
        latestData.pumpB = payload === '1' || payload === 'true';
        break;
      case TOPICS.PUMP_PH:
        latestData.pumpPh = payload === '1' || payload === 'true';
        break;
      case TOPICS.PUMP_MAIN:
        latestData.pumpMain = payload === '1' || payload === 'true';
        break;
      case TOPICS.SYSTEM_AUTO:
        latestData.autoMode = payload === '1' || payload === 'true';
        break;
      case TOPICS.SYSTEM_STATUS:
        console.log('[MQTT] ESP32 status:', payload);
        break;
    }
  });

  client.on('error', (err) => {
    console.error('[MQTT] Error:', err.message);
  });

  client.on('close', () => {
    console.log('[MQTT] Connection closed, will retry...');
  });
}

/**
 * Publish a message to a topic
 */
function publish(topic, payload) {
  if (!client || !client.connected) {
    console.warn('[MQTT] Not connected, cannot publish to', topic);
    return false;
  }
  const msg = typeof payload === 'object' ? JSON.stringify(payload) : String(payload);
  client.publish(topic, msg, { qos: 1 });
  return true;
}

/**
 * Get latest cached sensor/pump data
 */
function getLatestData() {
  return { ...latestData };
}

module.exports = { connect, publish, getLatestData, TOPICS };
