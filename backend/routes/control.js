/**
 * Control REST API Routes
 * POST /api/control/pump     - turn pump on/off
 * POST /api/control/target   - set EC/pH target
 * POST /api/control/auto     - start/stop auto dosing
 * POST /api/control/emergency - emergency stop
 */
const express = require('express');
const router = express.Router();
const mqttClient = require('../mqtt/client');
const { TOPICS } = mqttClient;

// POST /api/control/pump  { pump: "a"|"b"|"ph"|"main", state: true|false }
router.post('/pump', (req, res) => {
  const { pump, state } = req.body;
  const topicMap = {
    a:    TOPICS.PUMP_A,
    b:    TOPICS.PUMP_B,
    ph:   TOPICS.PUMP_PH,
    main: TOPICS.PUMP_MAIN,
  };

  const topic = topicMap[pump];
  if (!topic) {
    return res.status(400).json({ error: `Invalid pump. Use: ${Object.keys(topicMap).join(', ')}` });
  }

  const ok = mqttClient.publish(topic, state ? '1' : '0');
  res.json({ success: ok, pump, state: !!state });
});

// POST /api/control/target  { type: "ec"|"ph", value: 2.0 }
router.post('/target', (req, res) => {
  const { type, value } = req.body;

  if (type === 'ec') {
    mqttClient.publish(TOPICS.CONTROL_EC_TARGET, String(value));
  } else if (type === 'ph') {
    mqttClient.publish(TOPICS.CONTROL_PH_TARGET, String(value));
  } else {
    return res.status(400).json({ error: 'Invalid type. Use: ec, ph' });
  }

  res.json({ success: true, type, value });
});

// POST /api/control/auto  { enabled: true|false }
router.post('/auto', (req, res) => {
  const { enabled } = req.body;
  mqttClient.publish(TOPICS.SYSTEM_AUTO, enabled ? '1' : '0');
  res.json({ success: true, autoMode: !!enabled });
});

// POST /api/control/emergency
router.post('/emergency', (req, res) => {
  mqttClient.publish(TOPICS.SYSTEM_EMERGENCY, '1');
  res.json({ success: true, message: 'Emergency stop sent' });
});

module.exports = router;
