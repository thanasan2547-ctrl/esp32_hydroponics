/**
 * Sensor REST API Routes
 * GET /api/sensors/latest   - current sensor values (cached from MQTT)
 * GET /api/sensors/history  - historical data from InfluxDB
 */
const express = require('express');
const router = express.Router();
const mqttClient = require('../mqtt/client');
const influx = require('../db/influx');

// GET /api/sensors/latest
router.get('/latest', (req, res) => {
  const data = mqttClient.getLatestData();
  res.json({
    ec: data.ec,
    ph: data.ph,
    waterLevel: data.water,
    pumpA: data.pumpA,
    pumpB: data.pumpB,
    pumpPh: data.pumpPh,
    pumpMain: data.pumpMain,
    autoMode: data.autoMode,
    lastUpdate: data.lastUpdate,
  });
});

// GET /api/sensors/history?type=ec&range=1h&window=1m
router.get('/history', async (req, res) => {
  try {
    const { type = 'ec', range = '1h', window = '1m' } = req.query;

    // Validate sensor type
    const validTypes = ['ec', 'ph', 'water'];
    if (!validTypes.includes(type)) {
      return res.status(400).json({ error: `Invalid type. Use: ${validTypes.join(', ')}` });
    }

    // Validate range format (simple check)
    const rangeStr = range.startsWith('-') ? range : `-${range}`;

    const data = await influx.querySensorHistory(type, rangeStr, window);
    res.json({ sensor: type, range: rangeStr, window, count: data.length, data });
  } catch (err) {
    console.error('[API] History query error:', err.message);
    res.status(500).json({ error: 'Failed to query sensor history' });
  }
});

module.exports = router;
