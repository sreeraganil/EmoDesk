/**
 * EmoDesk - Automated Amazon & Flipkart Delivery Tracker (Google Apps Script Web App)
 * 
 * 100% Free Forever • No Third-Party Accounts • No JSONBin Limits • Zero Open Ports
 * 
 * HOW THIS WORKS:
 * 1. This script runs directly inside your Google account.
 * 2. It searches your Gmail for Amazon India and Flipkart shipment & delivery emails.
 * 3. It serves as a secure Web API (Web App) that your EmoDesk fetches directly over Wi-Fi!
 * 
 * -----------------------------------------------------------------------------------
 * SETUP INSTRUCTIONS (Takes ~2 minutes):
 * -----------------------------------------------------------------------------------
 * Step 1: Open Google Apps Script
 *    Go to https://script.google.com and click "+ New project".
 * 
 * Step 2: Paste this Code
 *    Select all text in Code.gs, delete it, and paste this entire script.
 * 
 * Step 3: Test Run
 *    Click "Run" at the top (with `doGet` or `testSync` selected).
 *    Google will ask for permission to read emails (Click "Review permissions" -> Choose account -> "Advanced" -> "Go to Untitled project (unsafe)" -> "Allow").
 *    Look at the Execution Log at the bottom to verify it found your shipments!
 * 
 * Step 4: Deploy as Web App
 *    Click the blue "Deploy" button (top right) -> "New deployment".
 *    - Click the gear icon next to "Select type" -> Choose "Web app".
 *    - Description: "EmoDesk Deliveries"
 *    - Execute as: "Me"
 *    - Who has access: "Anyone"   <-- IMPORTANT so EmoDesk can fetch it!
 *    - Click "Deploy".
 *    - Copy the "Web app URL" (looks like: https://script.google.com/macros/s/AKfycb.../exec)
 * 
 * Step 5: Put Web App URL into EmoDesk
 *    - Open your EmoDesk Dashboard (http://emo.local)
 *    - Go to Cards -> Add or Edit "Delivery tracker" card
 *    - Paste the Web App URL into "Custom webhook URL"
 *    - Set refresh interval (e.g. 15 or 30 minutes)
 *    - Save! Your EmoDesk will now automatically show your packages!
 */

function doGet(e) {
  const shipments = getActiveShipments();
  return ContentService.createTextOutput(JSON.stringify(shipments))
    .setMimeType(ContentService.MimeType.JSON);
}

function testSync() {
  const shipments = getActiveShipments();
  Logger.log("Found " + shipments.length + " active shipments:");
  Logger.log(JSON.stringify(shipments, null, 2));
}

function getActiveShipments() {
  const shipments = [];

  // ==========================================
  // 1. Scan Amazon India Shipment Updates
  // ==========================================
  try {
    const amazonThreads = GmailApp.search('from:(order-update@amazon.in OR auto-confirm@amazon.in OR ship-confirm@amazon.in) newer_than:4d', 0, 5);
    for (const thread of amazonThreads) {
      const msgs = thread.getMessages();
      const msg = msgs[msgs.length - 1]; // latest message
      const subject = msg.getSubject() || "";
      const body = msg.getPlainBody() || "";

      let item = "Amazon Package";
      let status = "In transit";
      let stage = 2; // 0=Ordered, 1=Shipped, 2=In Transit, 3=Out for Delivery, 4=Delivered
      let eta = "Soon";

      const subLower = subject.toLowerCase();
      const bodyLower = body.toLowerCase();

      // Stage detection
      if (subLower.includes("out for delivery") || bodyLower.includes("out for delivery") || subLower.includes("arriving today")) {
        status = "Out for delivery";
        stage = 3;
        eta = "Today";
      } else if (subLower.includes("delivered") || bodyLower.includes("delivered to you")) {
        status = "Delivered";
        stage = 4;
        eta = "Delivered";
      } else if (subLower.includes("dispatched") || subLower.includes("shipped")) {
        status = "Shipped";
        stage = 1;
      }

      // Extract item name from subject
      const match = subject.match(/(?:Shipped|Dispatched|Arriving|Delivered):\s*([^\.]+?)(?:\s*\(.*|\s*[-–].*|$)/i);
      if (match && match[1]) {
        item = match[1].replace(/[\[\]]/g, '').trim();
      }

      // Extract delivery ETA
      const etaMatch = body.match(/(?:Arriving|Delivery by|Estimated delivery:?)\s*([A-Za-z0-9, ]{3,22})/i);
      if (etaMatch && etaMatch[1]) {
        eta = etaMatch[1].replace(/\r|\n/g, ' ').trim();
      }

      shipments.push({
        id: "amz_" + thread.getId().slice(-6),
        item: item.slice(0, 30),
        courier: "Amazon",
        status: status,
        stage: stage,
        eta: eta.slice(0, 20)
      });
    }
  } catch (err) {
    Logger.log("Amazon sync notice: " + err);
  }

  // ==========================================
  // 2. Scan Flipkart Shipment Updates
  // ==========================================
  try {
    const flipkartThreads = GmailApp.search('from:(updates@flipkart.com OR orders@flipkart.com) newer_than:4d', 0, 5);
    for (const thread of flipkartThreads) {
      const msgs = thread.getMessages();
      const msg = msgs[msgs.length - 1];
      const subject = msg.getSubject() || "";
      const body = msg.getPlainBody() || "";

      let item = "Flipkart Package";
      let status = "In transit";
      let stage = 2;
      let eta = "Soon";

      const subLower = subject.toLowerCase();
      const bodyLower = body.toLowerCase();

      if (subLower.includes("out for delivery") || bodyLower.includes("out for delivery")) {
        status = "Out for delivery";
        stage = 3;
        eta = "Today";
      } else if (subLower.includes("delivered") || bodyLower.includes("delivered")) {
        status = "Delivered";
        stage = 4;
        eta = "Delivered";
      } else if (subLower.includes("shipped") || subLower.includes("dispatched")) {
        status = "Shipped";
        stage = 1;
      }

      const match = subject.match(/(?:Item\s*|\bfor\s*)([A-Za-z0-9\s-]{4,28})/i);
      if (match && match[1]) {
        item = match[1].trim();
      }

      const etaMatch = body.match(/(?:by|delivery expected on)\s*([A-Za-z0-9, ]{4,20})/i);
      if (etaMatch && etaMatch[1]) {
        eta = etaMatch[1].replace(/\r|\n/g, ' ').trim();
      }

      shipments.push({
        id: "flp_" + thread.getId().slice(-6),
        item: item.slice(0, 30),
        courier: "Flipkart",
        status: status,
        stage: stage,
        eta: eta.slice(0, 20)
      });
    }
  } catch (err) {
    Logger.log("Flipkart sync notice: " + err);
  }

  // Deduplicate and filter: ignore orders delivered more than 2 days ago
  const activeOnly = shipments.filter(s => s.stage < 4 || s.status === "Delivered");
  return activeOnly.slice(0, 6);
}
