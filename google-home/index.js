/**
 * Halo Gate - Google Smart Home Fulfillment
 * 
 * This Cloud Function handles Google Smart Home API requests and
 * forwards commands to the Halo ESP32-C6 hub via MQTT (Adafruit IO).
 * 
 * Devices:
 *   - Halo Light: LED ring with color, brightness, and effects
 *   - Halo Blinds: MoES Tuya Zigbee blinds
 */

const { smarthome } = require('actions-on-google');
const mqtt = require('mqtt');

// ============================================================================
// CONFIGURATION - Update these with your actual values
// ============================================================================

const CONFIG = {
  // Adafruit IO credentials (same as ESP32)
  ADAFRUIT_IO_USERNAME: process.env.ADAFRUIT_IO_USERNAME || 'YOUR_USERNAME',
  ADAFRUIT_IO_KEY: process.env.ADAFRUIT_IO_KEY || 'YOUR_KEY',
  
  // MQTT topics (must match ESP32 subscriptions)
  MQTT_TOPIC_COMMAND: 'commands',  // Will become: username/feeds/commands
  
  // Device IDs (used by Google Home)
  DEVICE_LIGHT_ID: 'halo-light-1',
  DEVICE_BLINDS_ID: 'halo-blinds-1',
};

// ============================================================================
// MQTT CLIENT
// ============================================================================

let mqttClient = null;

function getMqttClient() {
  if (mqttClient && mqttClient.connected) {
    return Promise.resolve(mqttClient);
  }
  
  return new Promise((resolve, reject) => {
    const client = mqtt.connect('mqtts://io.adafruit.com:8883', {
      username: CONFIG.ADAFRUIT_IO_USERNAME,
      password: CONFIG.ADAFRUIT_IO_KEY,
      clientId: `halo-google-home-${Date.now()}`,
      clean: true,
      connectTimeout: 10000,
    });
    
    client.on('connect', () => {
      console.log('MQTT connected to Adafruit IO');
      mqttClient = client;
      resolve(client);
    });
    
    client.on('error', (err) => {
      console.error('MQTT error:', err);
      reject(err);
    });
    
    // Timeout after 10s
    setTimeout(() => reject(new Error('MQTT connection timeout')), 10000);
  });
}

async function publishCommand(command) {
  const client = await getMqttClient();
  const topic = `${CONFIG.ADAFRUIT_IO_USERNAME}/feeds/${CONFIG.MQTT_TOPIC_COMMAND}`;
  
  return new Promise((resolve, reject) => {
    client.publish(topic, command, { qos: 1 }, (err) => {
      if (err) {
        console.error('MQTT publish error:', err);
        reject(err);
      } else {
        console.log(`Published: ${topic} = ${command}`);
        resolve();
      }
    });
  });
}

// ============================================================================
// DEVICE STATE (In production, use Firestore or similar)
// ============================================================================

// Simple in-memory state (resets on cold start)
// For production, use Firestore to persist state
const deviceState = {
  [CONFIG.DEVICE_LIGHT_ID]: {
    on: true,
    brightness: 100,
    color: { spectrumRgb: 0xFFFFFF },
    currentEffect: 'solid',
  },
  [CONFIG.DEVICE_BLINDS_ID]: {
    openPercent: 100,  // 100 = fully open
  },
};

// ============================================================================
// SMART HOME APP
// ============================================================================

const app = smarthome({
  debug: true,
});

/**
 * SYNC - Called when user links their account or says "sync my devices"
 * Returns the list of devices and their capabilities
 */
app.onSync(async (body, headers) => {
  console.log('SYNC request received');
  
  return {
    requestId: body.requestId,
    payload: {
      agentUserId: 'halo-user-1',  // Unique user ID in your system
      devices: [
        // ─────────────────────────────────────────────────────────────────
        // HALO LIGHT - LED Ring
        // ─────────────────────────────────────────────────────────────────
        {
          id: CONFIG.DEVICE_LIGHT_ID,
          type: 'action.devices.types.LIGHT',
          traits: [
            'action.devices.traits.OnOff',
            'action.devices.traits.Brightness',
            'action.devices.traits.ColorSetting',
            'action.devices.traits.Modes',
          ],
          name: {
            defaultNames: ['Halo Light'],
            name: 'Halo',
            nicknames: ['Halo Light', 'LED Ring', 'Halo Ring', 'The Ring'],
          },
          willReportState: false,
          roomHint: 'Office',  // Change to your room
          deviceInfo: {
            manufacturer: 'Halo',
            model: 'ESP32-C6 LED Hub',
            hwVersion: '1.0',
            swVersion: '1.0',
          },
          attributes: {
            // Color support
            colorModel: 'rgb',
            // Custom modes for effects
            availableModes: [
              {
                name: 'effect',
                name_values: [
                  { name_synonym: ['effect', 'animation', 'mode'], lang: 'en' },
                ],
                settings: [
                  { setting_name: 'solid', setting_values: [{ setting_synonym: ['solid', 'static', 'normal'], lang: 'en' }] },
                  { setting_name: 'rainbow', setting_values: [{ setting_synonym: ['rainbow', 'color cycle'], lang: 'en' }] },
                  { setting_name: 'breathing', setting_values: [{ setting_synonym: ['breathing', 'pulse', 'breathe'], lang: 'en' }] },
                  { setting_name: 'meteor', setting_values: [{ setting_synonym: ['meteor', 'shooting star', 'comet'], lang: 'en' }] },
                  { setting_name: 'wave', setting_values: [{ setting_synonym: ['wave', 'ocean'], lang: 'en' }] },
                  { setting_name: 'fusion', setting_values: [{ setting_synonym: ['fusion', 'blend'], lang: 'en' }] },
                  { setting_name: 'fire', setting_values: [{ setting_synonym: ['fire', 'flame', 'flames'], lang: 'en' }] },
                  { setting_name: 'candle', setting_values: [{ setting_synonym: ['candle', 'candlelight', 'flicker'], lang: 'en' }] },
                  { setting_name: 'chill', setting_values: [{ setting_synonym: ['chill', 'relaxing', 'calm'], lang: 'en' }] },
                ],
                ordered: false,
              },
            ],
          },
        },
        // ─────────────────────────────────────────────────────────────────
        // HALO BLINDS - Zigbee Blinds
        // ─────────────────────────────────────────────────────────────────
        {
          id: CONFIG.DEVICE_BLINDS_ID,
          type: 'action.devices.types.BLINDS',
          traits: [
            'action.devices.traits.OpenClose',
          ],
          name: {
            defaultNames: ['Halo Blinds'],
            name: 'Blinds',
            nicknames: ['Blinds', 'Window Blinds', 'Shades', 'Window Shades', 'The Blinds'],
          },
          willReportState: false,
          roomHint: 'Office',  // Change to your room
          deviceInfo: {
            manufacturer: 'MoES',
            model: 'Tuya Zigbee Blinds',
            hwVersion: '1.0',
            swVersion: '1.0',
          },
          attributes: {
            discreteOnlyOpenClose: false,
            openDirection: ['UP', 'DOWN'],
            commandOnlyOpenClose: false,
            queryOnlyOpenClose: false,
          },
        },
      ],
    },
  };
});

/**
 * QUERY - Called when Google asks "what's the status of X?"
 */
app.onQuery(async (body, headers) => {
  console.log('QUERY request:', JSON.stringify(body.inputs[0].payload.devices));
  
  const devices = {};
  
  for (const device of body.inputs[0].payload.devices) {
    const state = deviceState[device.id];
    
    if (device.id === CONFIG.DEVICE_LIGHT_ID) {
      devices[device.id] = {
        status: 'SUCCESS',
        online: true,
        on: state?.on ?? true,
        brightness: state?.brightness ?? 100,
        color: state?.color ?? { spectrumRgb: 0xFFFFFF },
        currentModeSettings: {
          effect: state?.currentEffect ?? 'solid',
        },
      };
    } else if (device.id === CONFIG.DEVICE_BLINDS_ID) {
      devices[device.id] = {
        status: 'SUCCESS',
        online: true,
        openPercent: state?.openPercent ?? 100,
      };
    } else {
      devices[device.id] = {
        status: 'ERROR',
        errorCode: 'deviceNotFound',
      };
    }
  }
  
  return {
    requestId: body.requestId,
    payload: { devices },
  };
});

/**
 * EXECUTE - Called when user gives a command like "turn on the lights"
 */
app.onExecute(async (body, headers) => {
  console.log('EXECUTE request:', JSON.stringify(body.inputs[0].payload.commands));
  
  const results = [];
  
  for (const command of body.inputs[0].payload.commands) {
    for (const device of command.devices) {
      const deviceId = device.id;
      
      for (const execution of command.execution) {
        try {
          const result = await executeCommand(deviceId, execution);
          results.push({
            ids: [deviceId],
            status: 'SUCCESS',
            states: result.states,
          });
        } catch (err) {
          console.error(`Execute error for ${deviceId}:`, err);
          results.push({
            ids: [deviceId],
            status: 'ERROR',
            errorCode: err.code || 'hardError',
          });
        }
      }
    }
  }
  
  return {
    requestId: body.requestId,
    payload: {
      commands: results,
    },
  };
});

/**
 * Execute a single command on a device
 */
async function executeCommand(deviceId, execution) {
  const { command, params } = execution;
  console.log(`Executing ${command} on ${deviceId} with params:`, params);
  
  // Initialize state if needed
  if (!deviceState[deviceId]) {
    deviceState[deviceId] = {};
  }
  
  // ─────────────────────────────────────────────────────────────────────────
  // LIGHT COMMANDS
  // ─────────────────────────────────────────────────────────────────────────
  if (deviceId === CONFIG.DEVICE_LIGHT_ID) {
    switch (command) {
      case 'action.devices.commands.OnOff': {
        const on = params.on;
        deviceState[deviceId].on = on;
        
        if (on) {
          await publishCommand('on');
        } else {
          await publishCommand('off');
        }
        
        return { states: { on } };
      }
      
      case 'action.devices.commands.BrightnessAbsolute': {
        const brightness = params.brightness;
        deviceState[deviceId].brightness = brightness;
        
        await publishCommand(`brightness:${brightness}`);
        
        return { states: { brightness } };
      }
      
      case 'action.devices.commands.ColorAbsolute': {
        const color = params.color;
        deviceState[deviceId].color = color;
        
        if (color.spectrumRgb !== undefined) {
          // Convert RGB integer to hex string
          const hex = color.spectrumRgb.toString(16).padStart(6, '0');
          await publishCommand(`color:${hex}`);
        }
        
        return { states: { color } };
      }
      
      case 'action.devices.commands.SetModes': {
        const effect = params.updateModeSettings?.effect;
        if (effect) {
          deviceState[deviceId].currentEffect = effect;
          await publishCommand(`effect:${effect}`);
        }
        
        return {
          states: {
            currentModeSettings: { effect },
          },
        };
      }
    }
  }
  
  // ─────────────────────────────────────────────────────────────────────────
  // BLINDS COMMANDS
  // ─────────────────────────────────────────────────────────────────────────
  if (deviceId === CONFIG.DEVICE_BLINDS_ID) {
    switch (command) {
      case 'action.devices.commands.OpenClose': {
        const openPercent = params.openPercent;
        deviceState[deviceId].openPercent = openPercent;
        
        // Map to ESP32 commands
        if (openPercent === 100) {
          await publishCommand('blinds:open');
        } else if (openPercent === 0) {
          await publishCommand('blinds:close');
        } else {
          await publishCommand(`blinds:${openPercent}`);
        }
        
        return { states: { openPercent } };
      }
    }
  }
  
  throw { code: 'functionNotSupported' };
}

/**
 * DISCONNECT - Called when user unlinks their account
 */
app.onDisconnect(async (body, headers) => {
  console.log('DISCONNECT request received');
  // Clean up user data if needed
  return {};
});

// ============================================================================
// CLOUD FUNCTION EXPORTS
// ============================================================================

// For Google Cloud Functions Gen2
// Entry point: smarthome (for halo-gate)
exports.smarthome = app;

// Entry point: authHandler (for halo-wall)
// Re-exported from auth.js so Cloud Functions can find it in the main file
exports.authHandler = require('./auth').authHandler;

