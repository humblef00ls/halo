/**
 * Halo Gate - OAuth2 Handler for Google Smart Home Account Linking
 *
 * This is a minimal OAuth implementation for testing.
 * For production, use Firebase Auth, Auth0, or proper OAuth server.
 *
 * Deploy this alongside the smarthome function.
 */

const crypto = require("crypto");

// ============================================================================
// CONFIGURATION (loaded lazily to not crash container at startup)
// ============================================================================

const REQUIRED_ENV_VARS = [
  "OAUTH_CLIENT_ID",
  "OAUTH_CLIENT_SECRET",
  "HALO_USERNAME",
  "HALO_PASSWORD",
];

let CONFIG = null;

function getConfig() {
  if (CONFIG) return CONFIG;

  const missing = REQUIRED_ENV_VARS.filter((v) => !process.env[v]);
  if (missing.length > 0) {
    throw new Error(
      `Missing required environment variables: ${missing.join(", ")}`
    );
  }

  CONFIG = {
    CLIENT_ID: process.env.OAUTH_CLIENT_ID,
    CLIENT_SECRET: process.env.OAUTH_CLIENT_SECRET,
    HALO_USERNAME: process.env.HALO_USERNAME,
    HALO_PASSWORD: process.env.HALO_PASSWORD,
  };

  return CONFIG;
}

// In-memory token storage (use Firestore for production)
const tokens = new Map();
const authCodes = new Map();

// ============================================================================
// HELPER: Parse URL-encoded body
// ============================================================================

function parseBody(req) {
  return new Promise((resolve) => {
    if (req.body) {
      // Already parsed
      resolve(req.body);
      return;
    }

    let data = "";
    req.on("data", (chunk) => {
      data += chunk;
    });
    req.on("end", () => {
      try {
        // Try JSON first
        resolve(JSON.parse(data));
      } catch {
        // Parse as URL-encoded
        const params = new URLSearchParams(data);
        const body = {};
        for (const [key, value] of params) {
          body[key] = value;
        }
        resolve(body);
      }
    });
  });
}

// ============================================================================
// LOGIN PAGE HTML
// ============================================================================

function getLoginPageHtml(redirect_uri, state) {
  return `
<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Link Halo to Google Home</title>
  <style>
    * { box-sizing: border-box; margin: 0; padding: 0; }
    body {
      font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
      background: linear-gradient(135deg, #1a1a2e 0%, #16213e 100%);
      min-height: 100vh;
      display: flex;
      align-items: center;
      justify-content: center;
      padding: 20px;
    }
    .card {
      background: rgba(255,255,255,0.95);
      border-radius: 16px;
      padding: 40px;
      max-width: 400px;
      width: 100%;
      box-shadow: 0 20px 60px rgba(0,0,0,0.3);
    }
    .logo {
      width: 80px;
      height: 80px;
      background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
      border-radius: 50%;
      margin: 0 auto 24px;
      display: flex;
      align-items: center;
      justify-content: center;
      font-size: 32px;
    }
    h1 {
      text-align: center;
      color: #1a1a2e;
      margin-bottom: 8px;
      font-size: 24px;
    }
    p {
      text-align: center;
      color: #666;
      margin-bottom: 32px;
      font-size: 14px;
    }
    input {
      width: 100%;
      padding: 14px 16px;
      border: 2px solid #e0e0e0;
      border-radius: 8px;
      font-size: 16px;
      margin-bottom: 16px;
      transition: border-color 0.2s;
    }
    input:focus {
      outline: none;
      border-color: #667eea;
    }
    button {
      width: 100%;
      padding: 16px;
      background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
      color: white;
      border: none;
      border-radius: 8px;
      font-size: 16px;
      font-weight: 600;
      cursor: pointer;
      transition: transform 0.2s, box-shadow 0.2s;
    }
    button:hover {
      transform: translateY(-2px);
      box-shadow: 0 8px 20px rgba(102, 126, 234, 0.4);
    }
  </style>
</head>
<body>
  <div class="card">
    <div class="logo">🌟</div>
    <h1>Link Halo to Google Home</h1>
    <p>Sign in to connect your Halo smart hub with Google Assistant</p>
    <form method="POST">
      <input type="hidden" name="redirect_uri" value="${redirect_uri || ""}">
      <input type="hidden" name="state" value="${state || ""}">
      <input type="hidden" name="action" value="login">
      <input type="text" name="username" placeholder="Username" required autocomplete="username">
      <input type="password" name="password" placeholder="Password" required autocomplete="current-password">
      <button type="submit">Link Account</button>
    </form>
  </div>
</body>
</html>
  `;
}

// ============================================================================
// MAIN CLOUD FUNCTION HANDLER
// ============================================================================

exports.authHandler = async (req, res) => {
  const path = req.path || req.url || "";
  const method = req.method;

  console.log(`Auth request: ${method} ${path}`);

  // ─────────────────────────────────────────────────────────────────────────
  // GET /authorize - Show login page
  // ─────────────────────────────────────────────────────────────────────────
  if (path.includes("/authorize") && method === "GET") {
    const { client_id, redirect_uri, state, response_type } = req.query;

    console.log("Authorization request:", {
      client_id,
      redirect_uri,
      state,
      response_type,
    });

    if (client_id !== getConfig().CLIENT_ID) {
      return res.status(400).send("Invalid client_id");
    }

    res.send(getLoginPageHtml(redirect_uri, state));
    return;
  }

  // ─────────────────────────────────────────────────────────────────────────
  // POST /authorize - Handle login form submission
  // ─────────────────────────────────────────────────────────────────────────
  if (path.includes("/authorize") && method === "POST") {
    const body = await parseBody(req);
    const { username, password, redirect_uri, state } = body;

    console.log("Login attempt:", { username });

    // Check credentials
    if (
      username !== getConfig().HALO_USERNAME ||
      password !== getConfig().HALO_PASSWORD
    ) {
      return res.status(401).send(`
        <html><body style="font-family: sans-serif; text-align: center; padding: 50px;">
          <h2>❌ Invalid credentials</h2>
          <p><a href="javascript:history.back()">Try again</a></p>
        </body></html>
      `);
    }

    // Generate authorization code
    const code = crypto.randomBytes(16).toString("hex");
    authCodes.set(code, {
      userId: "halo-user-1",
      expiresAt: Date.now() + 10 * 60 * 1000, // 10 minutes
    });

    // Redirect back to Google with auth code
    const redirectUrl = new URL(redirect_uri);
    redirectUrl.searchParams.set("code", code);
    redirectUrl.searchParams.set("state", state);

    console.log("Login success, redirecting...");
    res.redirect(redirectUrl.toString());
    return;
  }

  // ─────────────────────────────────────────────────────────────────────────
  // POST /token - Exchange auth code for tokens
  // ─────────────────────────────────────────────────────────────────────────
  if (path.includes("/token") && method === "POST") {
    const body = await parseBody(req);
    const { grant_type, code, refresh_token, client_id, client_secret } = body;

    console.log("Token request:", { grant_type, client_id });

    // Validate client
    if (
      client_id !== getConfig().CLIENT_ID ||
      client_secret !== getConfig().CLIENT_SECRET
    ) {
      console.log("Invalid client credentials");
      return res.status(401).json({ error: "invalid_client" });
    }

    if (grant_type === "authorization_code") {
      // Exchange auth code for tokens
      const authData = authCodes.get(code);
      if (!authData || authData.expiresAt < Date.now()) {
        console.log("Invalid or expired auth code");
        return res.status(400).json({ error: "invalid_grant" });
      }

      authCodes.delete(code);

      // Generate tokens
      const accessToken = crypto.randomBytes(32).toString("hex");
      const newRefreshToken = crypto.randomBytes(32).toString("hex");

      tokens.set(accessToken, {
        userId: authData.userId,
        expiresAt: Date.now() + 3600 * 1000, // 1 hour
      });
      tokens.set(newRefreshToken, {
        userId: authData.userId,
        isRefreshToken: true,
      });

      console.log("Tokens generated successfully");
      return res.json({
        token_type: "Bearer",
        access_token: accessToken,
        refresh_token: newRefreshToken,
        expires_in: 3600,
      });
    }

    if (grant_type === "refresh_token") {
      // Refresh the access token
      const tokenData = tokens.get(refresh_token);
      if (!tokenData || !tokenData.isRefreshToken) {
        console.log("Invalid refresh token");
        return res.status(400).json({ error: "invalid_grant" });
      }

      const accessToken = crypto.randomBytes(32).toString("hex");
      tokens.set(accessToken, {
        userId: tokenData.userId,
        expiresAt: Date.now() + 3600 * 1000,
      });

      console.log("Token refreshed successfully");
      return res.json({
        token_type: "Bearer",
        access_token: accessToken,
        expires_in: 3600,
      });
    }

    return res.status(400).json({ error: "unsupported_grant_type" });
  }

  // ─────────────────────────────────────────────────────────────────────────
  // Health check / root
  // ─────────────────────────────────────────────────────────────────────────
  if (path === "/" || path === "") {
    return res.send("Halo Gate OAuth Server - OK");
  }

  res.status(404).send("Not found");
};
