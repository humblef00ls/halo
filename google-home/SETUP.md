# Google Home Integration Setup Guide

This guide walks you through setting up native Google Home control for your Halo ESP32-C6 hub.

> **Note:** This uses the [Google Smart Home API](https://developers.google.com/assistant/smarthome/overview),
> which is fully supported (unlike Conversational Actions which were sunset in June 2023).

## What You'll Get

After setup, you can say things like:

- "Hey Google, turn on the Halo"
- "Hey Google, set Halo to 50%"
- "Hey Google, make Halo blue"
- "Hey Google, set Halo to rainbow mode"
- "Hey Google, open the blinds"
- "Hey Google, close the blinds to 30%"

## Architecture

```
┌──────────────┐     ┌───────────────────┐     ┌──────────────┐
│ Google Home  │────▶│  Cloud Function   │────▶│  Adafruit IO │
│   / Nest     │     │  (fulfillment)    │     │    (MQTT)    │
└──────────────┘     └───────────────────┘     └──────┬───────┘
                                                      │
                                                      ▼
                                               ┌──────────────┐
                                               │  ESP32-C6    │
                                               │   (Halo)     │
                                               └──────────────┘
```

---

## Prerequisites: gcloud CLI Setup

Before starting, make sure you have the gcloud CLI installed and configured.

### Install gcloud CLI (if needed)

```bash
# macOS
brew install --cask google-cloud-sdk

# Or download from: https://cloud.google.com/sdk/docs/install
```

### Switch Account (if you have multiple Google accounts)

```bash
# See what account you're currently using
gcloud auth list

# Login with a different account (opens browser)
gcloud auth login

# Verify you're using the right account
gcloud config get-value account
```

### Switch Project (if you have multiple projects)

```bash
# See current project
gcloud config get-value project

# List all your projects
gcloud projects list

# Switch to a different project
gcloud config set project YOUR_PROJECT_ID
```

### Quick Reference: Full Account + Project Switch

```bash
# One-liner to switch everything
gcloud config set account your-email@gmail.com && gcloud config set project a-humblef00ls-ark

# Verify current configuration
gcloud config list
```

---

## Step 1: Create Google Cloud Project

### Option A: Via Web Console

1. Go to [Google Cloud Console](https://console.cloud.google.com/)
2. Click the project dropdown (top left) → **New Project**
3. Enter a name (e.g., "The Domain" or "Halo Gate")
4. Note your **Project ID** (e.g., `a-humblef00ls-ark` or `halo-smart-home-12345`)

### Option B: Via gcloud CLI

```bash
# Create new project
gcloud projects create a-humblef00ls-ark --name="The Domain"

# Switch to it
gcloud config set project a-humblef00ls-ark

# Verify
gcloud config get-value project
```

> **Tip:** Project IDs must be globally unique. If `a-humblef00ls-ark` is taken, try `a-humblef00ls-ark-12345` or similar.

## Step 2: Enable Billing & Required APIs

### 2.1 Enable Billing

Cloud Functions require a billing account (but personal use stays well within free tier):

1. Go to [Cloud Console Billing](https://console.cloud.google.com/billing)
2. Link a billing account to your project
3. Or via CLI:

   ```bash
   # List billing accounts
   gcloud billing accounts list

   # Link to project
   gcloud billing projects link a-humblef00ls-ark --billing-account=XXXXXX-XXXXXX-XXXXXX
   ```

> **Cost:** For personal smart home use, you'll likely stay in the free tier (2 million invocations/month free).

### 2.2 Enable Required APIs

```bash
# Make sure you're in the right project
gcloud config get-value project

# Enable the APIs
gcloud services enable cloudfunctions.googleapis.com
gcloud services enable cloudbuild.googleapis.com
gcloud services enable actions.googleapis.com
```

Or enable via [Cloud Console APIs](https://console.cloud.google.com/apis/library):

- Cloud Functions API
- Cloud Build API
- Actions API

## Step 3: Deploy Cloud Functions

### 3.1 Configure Environment Variables

First, generate your OAuth credentials (you create these yourself):

```bash
# Generate a secure random secret
openssl rand -hex 32
# Example output: a3f8b2c1d4e5f6a7b8c9d0e1f2a3b4c5d6e7f8a9b0c1d2e3f4a5b6c7d8e9f0a1
```

Create a `.env` file from the template:

```bash
cd google-home
cp env.example .env
```

Edit `.env` with your values:

```bash
# Adafruit IO (same credentials as your ESP32's credentials.h)
ADAFRUIT_IO_USERNAME=your_adafruit_username
ADAFRUIT_IO_KEY=aio_xxxxxxxxxxxxxxxxxxxx

# OAuth credentials (you make these up, use the same values in Actions Console later)
OAUTH_CLIENT_ID=halo-google-home-client
OAUTH_CLIENT_SECRET=paste-your-generated-secret-here

# Login credentials for account linking (pick a username/password)
HALO_USERNAME=admin
HALO_PASSWORD=pick-a-secure-password
```

> **Important:** The `.env` file is gitignored. Never commit secrets to git!

### 3.2 Deploy the Functions

```bash
cd google-home

# Install dependencies
npm install

# Verify you're in the right project
gcloud config get-value project
```

Deploy the Smart Home fulfillment function:

```bash
gcloud functions deploy halo-gate \
  --gen2 \
  --runtime nodejs22 \
  --trigger-http \
  --allow-unauthenticated \
  --entry-point smarthome \
  --region us-central1 \
  --set-env-vars "ADAFRUIT_IO_USERNAME=YOUR_USERNAME,ADAFRUIT_IO_KEY=YOUR_KEY"
```

Deploy the OAuth handler function:

```bash
gcloud functions deploy halo-wall \
  --gen2 \
  --runtime nodejs22 \
  --trigger-http \
  --allow-unauthenticated \
  --entry-point authHandler \
  --region us-central1 \
  --set-env-vars "OAUTH_CLIENT_ID=halo-google-home-client,OAUTH_CLIENT_SECRET=YOUR_SECRET,HALO_USERNAME=admin,HALO_PASSWORD=YOUR_PASSWORD"
```

> **Replace** `YOUR_USERNAME`, `YOUR_KEY`, `YOUR_SECRET`, `YOUR_PASSWORD` with your actual values from `.env`

### 3.3 Note Your Function URLs

After deployment, gcloud will print the URLs. They'll look like:

```
https://us-central1-a-humblef00ls-ark.cloudfunctions.net/halo-gate

https://us-central1-a-humblef00ls-ark.cloudfunctions.net/halo-wall
```

Save these - you'll need them for the Actions Console.

You can also find them later:

```bash
gcloud functions describe halo-gate --region us-central1 --format='value(url)'
gcloud functions describe halo-wall --region us-central1 --format='value(url)'
```

## Step 4: Create Cloud-to-Cloud Integration

> **Note:** Google has moved Smart Home development to the **Google Home Developer Console**.
> The old Actions Console is deprecated for new Smart Home projects.

### 4.1 Create a Project in Google Home Developer Console

1. Go to [Google Home Developer Console](https://console.home.google.com/projects)
2. Click **Create project**
3. Enter a project name (e.g., "Halo Gate")
4. Click **Create project**

### 4.2 Add Cloud-to-Cloud Integration

1. On your project's home page, find the **Cloud-to-cloud** section
2. Click **Add cloud-to-cloud integration**
3. Enter integration details:
   - **Integration name:** `Halo Gate`
   - **Device types:** Select `Light` and `Blinds`

### 4.3 Configure Fulfillment

1. In the integration settings, find **Setup** → **Cloud fulfillment**
2. Enter your fulfillment URL:
   ```
   https://us-central1-a-humblef00ls-ark.cloudfunctions.net/halo-gate
   ```

### 4.4 Configure Account Linking

1. In the integration settings, find **Setup** → **Account linking**
2. Select **OAuth** as the linking type
3. Fill in:

   | Field                 | Value                                                                          |
   | --------------------- | ------------------------------------------------------------------------------ |
   | Client ID             | `halo-google-home-client`                                                      |
   | Client secret         | Your generated secret (same as deployed env vars)                              |
   | Authorization URL     | `https://us-central1-a-humblef00ls-ark.cloudfunctions.net/halo-wall/authorize` |
   | Token URL             | `https://us-central1-a-humblef00ls-ark.cloudfunctions.net/halo-wall/token`     |
   | Authentication method | HTTP Basic (recommended) or Body                                               |

4. Click **Save**

> **Critical:** The Client ID and Client Secret must **exactly match** what you deployed with `--set-env-vars`!

### 4.5 Enable the Integration for Testing

1. In the integration settings, find **Test** or **Launch**
2. Enable testing mode for your account
3. Your account email should be listed as a test user

---

## Step 5: Link Your Account & Test

### 5.1 Link in Google Home App

1. Open the **Google Home** app on your phone
2. Tap **+** (Add) → **Set up device** → **Works with Google**
3. Search for `[test] Halo Gate` (test integrations have [test] prefix)
4. Tap it to start account linking
5. Sign in with your Halo credentials:
   - Username: `admin` (or whatever you set in HALO_USERNAME)
   - Password: `1234` (or whatever you set in HALO_PASSWORD)
6. After successful auth, your devices should appear in Google Home!

### 5.2 Test Voice Commands

Try saying:

- "Hey Google, sync my devices" (refreshes device list)
- "Hey Google, turn on Halo"
- "Hey Google, set Halo to 50%"
- "Hey Google, set Halo to blue"
- "Hey Google, open the blinds"
- "Hey Google, close the blinds to 30%"

### 5.3 Use Google Home Playground (Optional)

For debugging, use [Google Home Playground](https://developers.home.google.com/tools/home-playground):

- Sign in with the same account
- See your devices and test commands
- View SYNC/QUERY/EXECUTE requests

### 5.4 Run Test Suite (Optional)

For formal testing, use [Google Home Test Suite](https://developers.home.google.com/cloud-to-cloud/tools/test-suite):

- Validates your integration
- Required for certification (if publishing)

---

## Step 6: Submit for Certification (Optional)

For **personal use**, testing mode works indefinitely. No need to certify.

For **public publishing**:

1. Go to [Google Home Developer Console](https://console.home.google.com/projects)
2. Select your project → **Cloud-to-cloud** → **Certify**
3. Complete the certification checklist:
   - Privacy policy URL
   - Terms of service URL
   - Test Suite results (must pass 100%)
   - Brand assets
4. Submit for review

---

## Troubleshooting

### "Device not responding"

1. Check Cloud Function logs:

   ```bash
   gcloud functions logs read halo-gate --limit=50
   ```

2. Verify MQTT connection:
   - Check Adafruit IO credentials
   - Verify ESP32 is subscribed to the same feed

### "Account linking failed"

1. Check auth function logs:

   ```bash
   gcloud functions logs read halo-wall --limit=50
   ```

2. Verify OAuth settings match between Actions Console and .env

### Commands not reaching ESP32

1. Test MQTT directly via Adafruit IO web interface
2. Check ESP32 serial output for incoming commands
3. Verify topic names match

---

## Extending the Integration

### Adding Custom Commands

Edit `index.js` to add new traits or modes:

```javascript
// In the SYNC handler, add to availableModes:
{ setting_name: 'party', setting_values: [{ setting_synonym: ['party', 'disco'], lang: 'en' }] },

// In executeCommand, handle it:
case 'action.devices.commands.SetModes': {
  const effect = params.updateModeSettings?.effect;
  if (effect === 'party') {
    await publishCommand('effect:party');
  }
  // ...
}
```

### Adding More Devices

Add new devices to the SYNC response and handle their commands in EXECUTE.

---

## Security Notes

- **Change default passwords** in production
- Use **Secret Manager** for credentials instead of env vars
- Consider **Firebase Auth** for proper OAuth
- The account linking tokens are stored in-memory (reset on cold start)
  - For production, use **Firestore** for persistence
