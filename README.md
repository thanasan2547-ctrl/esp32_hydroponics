# 🍅 Smart Hydroponics - Cloud Architecture

ESP32-based hydroponics controller with cloud monitoring and control via MQTT.

## Architecture

```
┌──────────┐     MQTT (TLS 8883)     ┌──────────────┐
│  ESP32   │ ──────────────────────> │  HiveMQ Cloud │
│ Firmware │ <────────────────────── │    Broker     │
└──────────┘                         └──────┬───────┘
                                            │
                              ┌─────────────┼─────────────┐
                              │             │             │
                         WSS :8884     MQTTS :8883        │
                              │             │             │
                      ┌───────▼──┐   ┌──────▼─────┐      │
                      │ Frontend │   │  Backend   │      │
                      │ (Vercel) │   │ (Node.js)  │      │
                      └──────────┘   └──────┬─────┘      │
                                            │             │
                                     ┌──────▼─────┐      │
                                     │  InfluxDB  │      │
                                     │   Cloud    │      │
                                     └────────────┘
```

## MQTT Topic Structure

| Topic | Direction | Description |
|-------|-----------|-------------|
| `hydroponics/sensor/ec` | ESP32 → Cloud | EC value (mS/cm) |
| `hydroponics/sensor/ph` | ESP32 → Cloud | pH value |
| `hydroponics/sensor/water` | ESP32 → Cloud | Water level (%) |
| `hydroponics/pump/a` | Bidirectional | Pump A state (`1`/`0`) |
| `hydroponics/pump/b` | Bidirectional | Pump B state (`1`/`0`) |
| `hydroponics/pump/ph` | Bidirectional | pH Down pump state |
| `hydroponics/pump/main` | Bidirectional | Main pump state |
| `hydroponics/control/ec_target` | Cloud → ESP32 | EC target setpoint |
| `hydroponics/control/ph_target` | Cloud → ESP32 | pH target setpoint |
| `hydroponics/system/auto` | Bidirectional | Auto dosing (`1`/`0`) |
| `hydroponics/system/emergency` | Cloud → ESP32 | Emergency stop |
| `hydroponics/system/status` | ESP32 → Cloud | LWT online/offline |
| `hydroponics/control/calibrate` | Cloud → ESP32 | Calibration commands |

## Project Structure

```
esp32_hydroponics/
├── project_esp32_tomato_hydroponics/
│   └── project_esp32_tomato_hydroponics.ino   # ESP32 firmware
├── backend/
│   ├── server.js              # Express entry point
│   ├── mqtt/client.js         # MQTT connection module
│   ├── db/influx.js           # InfluxDB read/write
│   ├── routes/
│   │   ├── sensors.js         # GET /api/sensors/latest, /history
│   │   └── control.js         # POST /api/control/pump, /target, /auto, /emergency
│   ├── package.json
│   ├── .env.example
│   └── .env                   # Your secrets (gitignored)
├── frontend/
│   ├── public/
│   │   └── index.html         # Dashboard (deploys to Vercel)
│   ├── vercel.json            # Vercel deployment config
│   └── package.json
├── cloud_version_web.html     # Original standalone dashboard
└── README.md
```

---

## Deployment Guide

### 1. HiveMQ Cloud (MQTT Broker)

1. Go to [hivemq.com/cloud](https://www.hivemq.com/cloud/) and create a free cluster.
2. Note your **broker URL** (e.g. `8218cf51f5de4ac5a776aa0efb931888.s1.eu.hivemq.cloud`).
3. Create credentials under **Access Management**:
   - Username: `tomato-esp32`
   - Password: `tomato_project_Y3`
4. Ports:
   - **8883** — MQTT over TLS (ESP32 + Backend)
   - **8884** — WebSocket over TLS (Frontend)

### 2. InfluxDB Cloud (Time-Series Database)

1. Sign up at [cloud2.influxdata.com](https://cloud2.influxdata.com/).
2. Create a **bucket** named `hydroponics`.
3. Generate an **API Token** with read/write access to that bucket.
4. Note your:
   - **URL**: e.g. `https://us-east-1-1.aws.cloud2.influxdata.com`
   - **Org**: your organization name
   - **Token**: the generated token
5. Update `backend/.env` with these values.

#### InfluxDB Schema

- **Measurement**: `sensor_data`
- **Tags**: `sensor` (ec | ph | water), `device` (esp32-tomato)
- **Field**: `value` (float)
- **Timestamp**: auto (second precision)

Example Flux query:
```flux
from(bucket: "hydroponics")
  |> range(start: -1h)
  |> filter(fn: (r) => r._measurement == "sensor_data")
  |> filter(fn: (r) => r.sensor == "ec")
  |> aggregateWindow(every: 1m, fn: mean, createEmpty: false)
```

### 3. Backend (Node.js + Express)

```bash
cd backend
npm install
```

Edit `.env` with your actual credentials, then:

```bash
# Development
npm run dev

# Production
npm start
```

**REST API Endpoints:**

| Method | Endpoint | Description |
|--------|----------|-------------|
| GET | `/api/sensors/latest` | Current sensor values (cached) |
| GET | `/api/sensors/history?type=ec&range=1h&window=1m` | Historical data |
| POST | `/api/control/pump` | `{ pump: "a", state: true }` |
| POST | `/api/control/target` | `{ type: "ec", value: 2.0 }` |
| POST | `/api/control/auto` | `{ enabled: true }` |
| POST | `/api/control/emergency` | Trigger emergency stop |
| GET | `/api/health` | Health check |

**Deploy Backend** (recommended: [Render](https://render.com) or [Railway](https://railway.app)):
1. Push `backend/` to a Git repo.
2. Set environment variables from `.env`.
3. Build command: `npm install`
4. Start command: `node server.js`

### 4. Frontend (Vercel)

1. Update `frontend/public/index.html`:
   - Set `API_BASE` to your deployed backend URL (e.g. `https://your-backend.onrender.com`).
2. Update `frontend/vercel.json`:
   - Replace the rewrite destination URL with your backend URL.

```bash
cd frontend
npx vercel --prod
```

Or connect the `frontend/` directory to Vercel via GitHub for auto-deploys.

### 5. ESP32 Firmware

1. Open `project_esp32_tomato_hydroponics.ino` in Arduino IDE.
2. Install libraries:
   - `PubSubClient`
   - `ArduinoJson`
   - `U8g2`
3. Update WiFi credentials if needed.
4. Upload to ESP32.

---

## Pin Map

| GPIO | Function |
|------|----------|
| 5 | LCD CS |
| 12 | Relay Pump B |
| 13 | Relay Pump A |
| 14 | Relay Main Pump / pH Down |
| 18 | LCD CLK |
| 22 | LCD RST |
| 23 | LCD DATA |
| 25 | Ultrasonic TRIG |
| 26 | Ultrasonic ECHO |
| 32 | Auto Dosing Button |
| 34 | EC Sensor (ADC) |
| 35 | pH Sensor (ADC) |
