/**
 * InfluxDB Module
 * Handles writing sensor data and querying history
 */
const { InfluxDB, Point } = require('@influxdata/influxdb-client');

let writeApi = null;
let queryApi = null;

function init() {
  const url = process.env.INFLUX_URL;
  const token = process.env.INFLUX_TOKEN;
  const org = process.env.INFLUX_ORG;
  const bucket = process.env.INFLUX_BUCKET;

  if (!url || !token || !org || !bucket) {
    console.warn('[InfluxDB] Missing config — history storage disabled');
    return false;
  }

  const influx = new InfluxDB({ url, token });
  writeApi = influx.getWriteApi(org, bucket, 's');  // second precision
  queryApi = influx.getQueryApi(org);

  writeApi.useDefaultTags({ device: 'esp32-tomato' });
  console.log('[InfluxDB] Connected to', url, '| bucket:', bucket);
  return true;
}

/**
 * Write a sensor reading to InfluxDB
 * @param {string} sensorType - 'ec' | 'ph' | 'water'
 * @param {number} value
 */
function writeSensorData(sensorType, value) {
  if (!writeApi) return;
  const point = new Point('sensor_data')
    .tag('sensor', sensorType)
    .floatField('value', value);
  writeApi.writePoint(point);
}

/**
 * Flush pending writes (call periodically or on shutdown)
 */
async function flush() {
  if (!writeApi) return;
  try {
    await writeApi.flush();
  } catch (err) {
    console.error('[InfluxDB] Flush error:', err.message);
  }
}

/**
 * Query sensor history
 * @param {string} sensorType - 'ec' | 'ph' | 'water'
 * @param {string} range - Flux duration string, e.g. '-1h', '-24h', '-7d'
 * @param {string} [aggregateWindow] - e.g. '1m', '5m', '1h'
 * @returns {Promise<Array<{time: string, value: number}>>}
 */
async function querySensorHistory(sensorType, range = '-1h', aggregateWindow = '1m') {
  if (!queryApi) return [];

  const bucket = process.env.INFLUX_BUCKET;
  const query = `
    from(bucket: "${bucket}")
      |> range(start: ${range})
      |> filter(fn: (r) => r._measurement == "sensor_data")
      |> filter(fn: (r) => r.sensor == "${sensorType}")
      |> filter(fn: (r) => r._field == "value")
      |> aggregateWindow(every: ${aggregateWindow}, fn: mean, createEmpty: false)
      |> yield(name: "mean")
  `;

  const results = [];
  return new Promise((resolve, reject) => {
    queryApi.queryRows(query, {
      next(row, tableMeta) {
        const obj = tableMeta.toObject(row);
        results.push({
          time: obj._time,
          value: parseFloat(obj._value.toFixed(3)),
        });
      },
      error(err) {
        console.error('[InfluxDB] Query error:', err.message);
        reject(err);
      },
      complete() {
        resolve(results);
      },
    });
  });
}

module.exports = { init, writeSensorData, flush, querySensorHistory };
