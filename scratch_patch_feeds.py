import re

path = "EmoDesk/feeds.cpp"
with open(path, "r") as f:
    content = f.read()

cricLoop_code = """
static void cricLoop(uint32_t now) {
  if ((int32_t)(now - nextCric) < 0) return;
  LOCK();
  String url = cfg["cric"]["url"] | "";
  String team = cfg["cric"]["team"] | "";
  url.trim();
  int liveMin = cfg["cric"]["liveMin"] | 3, idleMin = cfg["cric"]["idleMin"] | 60;
  bool alW = cfg["cric"]["wicket"] | true, alR = cfg["cric"]["result"] | true, sp = cfg["cric"]["speak"] | false;
  bool need = cardsHaveType("cricket") || cardsHaveType("cricfix");
  UNLOCK();
  if (!url.length() || !need) { nextCric = now + 60000; return; }
  
  if (url.endsWith("/api/cricket")) url = url.substring(0, url.length() - 12);
  String reqUrl = url + "/api/cricket/all";
  if (team.length()) reqUrl += "?team=" + urlEncode(team.c_str());

  JsonDocument filter;
  JsonObject m = filter.add<JsonObject>();
  m["title"] = true;
  m["status"] = true;
  m["statusText"] = true;
  m["format"] = true;
  m["startTime"] = true;
  JsonObject t = m["teams"].add<JsonObject>();
  t["name"] = true;
  t["short"] = true;
  JsonObject inn = t["innings"].add<JsonObject>();
  inn["runs"] = true;
  inn["wickets"] = true;
  inn["overs"] = true;

  JsonDocument doc;
  bool ok = httpJson(reqUrl, doc, &filter);
  if (!ok || !doc.is<JsonArray>()) {
    LOCK();
    safeCopy(cric.err, ok ? "API error" : "request failed", sizeof(cric.err));
    UNLOCK();
    nextCric = now + 15 * 60000UL;
    return;
  }
  CricData* nd = (CricData*)calloc(1, sizeof(CricData));
  if (!nd) return;
  JsonArray arr = doc.as<JsonArray>();

  for (int pass = 0; pass < 3; pass++) {
    for (JsonObject m : arr) {
      if (nd->n >= 8) break;
      const char* st = m["status"] | "";
      bool live = !strcmp(st, "live");
      bool mine = team.length() && icontains(m["title"] | "", team.c_str());
      if ((pass == 0 && mine) || (pass == 1 && !mine && live) || (pass == 2 && !mine && !live)) parseMatch(m, nd->m[nd->n++]);
    }
  }
  nd->hits = 0;
  nd->limit = 0;
  cricHits = max(cricHits, nd->hits);
  nd->ok = true;
  nd->ts = time(nullptr);

  bool anyLive = false;
  LOCK();
  for (int i = 0; i < nd->n; i++) {
    CricMatch& nm = nd->m[i];
    bool mine = team.length() && icontains(nm.name, team.c_str());
    if (!mine) continue;
    if (cricLive(nm)) anyLive = true;
    for (int j = 0; j < cric.n; j++) {
      CricMatch& om = cric.m[j];
      if (strcmp(om.name, nm.name)) continue;
      if (alW && nm.ni && nm.ni == om.ni && nm.inn[nm.ni - 1].w > om.inn[om.ni - 1].w) {
        CricInn& in = nm.inn[nm.ni - 1];
        bool ours = false;   // did OUR team lose the wicket?
        if (!strcmp(in.team, nm.t1)) ours = icontains(nm.t1n, team.c_str());
        else if (!strcmp(in.team, nm.t2)) ours = icontains(nm.t2n, team.c_str());
        char b[60];
        snprintf(b, sizeof(b), "%s %d/%d (%.1f)", in.team, in.r, in.w, in.o);
        notify("WICKET!", b, ours ? E_SAD : E_EXCITED, ours ? "sfx:error" : "sfx:goal", EV_SPORTS, sp, 12);
      }
      if (alR && om.started && !om.ended && nm.ended) {
        notify("RESULT", nm.status, E_HAPPY, "sfx:goal", EV_SPORTS, sp, 15);
      }
    }
  }
  memcpy(&cric, nd, sizeof(CricData));
  UNLOCK();
  free(nd);
  nextCric = now + (anyLive ? liveMin : idleMin) * 60000UL * powerScale();
}
"""

# Replace everything from "static void parseMatch...}" down to "void tgQueue"
start_idx = content.find("static void parseMatch")
end_idx = content.find("void tgQueue", start_idx)

# We just need to keep parseMatch and then insert cricLoop before tgQueue
# Let's extract parseMatch correctly. We know it ends with "  }\n"
parseMatch_end = content.find("  }", start_idx) + 3

# Wait, parseMatch has nested braces. 
# Better: regex just replace everything between parseMatch end and tgQueue.
# Let's just reconstruct that block.

new_content = content[:start_idx] + """static void parseMatch(JsonObject m, CricMatch& c) {
    memset(&c, 0, sizeof(c));
    safeCopy(c.name, m["title"] | "", sizeof(c.name));
    safeCopy(c.status, m["statusText"] | "", sizeof(c.status));
    safeCopy(c.type, m["format"] | "", sizeof(c.type));
    const char* st = m["status"] | "";
    c.started = !strcmp(st, "live") || !strcmp(st, "finished");
    c.ended = !strcmp(st, "finished") || !strcmp(st, "cancelled");
    c.start = parseISO(m["startTime"] | "");
    JsonArray teams = m["teams"];
    safeCopy(c.t1n, teams[0]["name"] | "", sizeof(c.t1n));
    safeCopy(c.t2n, teams[1]["name"] | "", sizeof(c.t2n));
    safeCopy(c.t1, teams[0]["short"] | "?", sizeof(c.t1));
    safeCopy(c.t2, teams[1]["short"] | "?", sizeof(c.t2));
    for (int i = 0; i < 2; i++) {
      for (JsonObject s : teams[i]["innings"].as<JsonArray>()) {
        if (c.ni >= 4) break;
        CricInn& in = c.inn[c.ni++];
        in.r = s["runs"] | 0;
        in.w = s["wickets"] | 0;
        in.o = s["overs"] | 0.0f;
        safeCopy(in.team, i == 0 ? c.t1 : c.t2, sizeof(in.team));
      }
    }
}
""" + cricLoop_code + "\n" + content[end_idx:]

with open(path, "w") as f:
    f.write(new_content)
