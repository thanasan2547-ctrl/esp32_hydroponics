/**
 * Hydroponics Backend Server
 * - Connects to HiveMQ Cloud via MQTT
 * - Stores sensor data in InfluxDB
 * - Provides REST API for frontend
 */
require('dotenv').config();

const express = require('express');
const cors = require('cors');
const mqttClient = require('./mqtt/client');
const influx = require('./db/influx');
const sensorRoutes = require('./routes/sensors');
const controlRoutes = require('./routes/control');

const app = express();
const PORT = process.env.PORT || 3001;

// ---- Middleware ----
app.use(cors({
  origin: process.env.FRONTEND_URL || '*',
  methods: ['GET', 'POST'],
}));
app.use(express.json());

// ---- Routes ----
app.use('/api/sensors', sensorRoutes);
app.use('/api/control', controlRoutes);

// Health check
app.get('/api/health', (req, res) => {
  res.json({ status: 'ok', uptime: process.uptime() });
});

// ---- Initialize Services ----
const influxReady = influx.init();

mqttClient.connect((sensorType, value) => {
  // Callback: every time a sensor value arrives via MQTT, store it in InfluxDB
  if (influxReady) {
    influx.writeSensorData(sensorType, value);
  }
});

// Flush InfluxDB writes every 10 seconds
setInterval(() => {
  if (influxReady) influx.flush();
}, 10000);

// ---- Start Server ----
app.listen(PORT, () => {
  console.log(`[Server] Hydroponics backend running on port ${PORT}`);
  console.log(`[Server] REST API: http://localhost:${PORT}/api/sensors/latest`);
});

// Graceful shutdown
process.on('SIGTERM', async () => {
  console.log('[Server] Shutting down...');
  if (influxReady) await influx.flush();
  process.exit(0);
});
