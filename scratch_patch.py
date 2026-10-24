import re

def update_cards_weather_sport():
    path = "EmoDesk/cards_weather_sport.cpp"
    with open(path, "r") as f:
        content = f.read()

    # Update football_fetch
    fetch_old = r"""static bool football_fetch\(JsonObject p, void\* d\) \{.*?FootData nd;"""
    fetch_new = """static bool football_fetch(JsonObject p, void* d) {
    FootData* fd = (FootData*)d;
    FootData old = *fd;
    String league = p["league"] | "eng.1";
    String team = p["team"] | "";
    bool alerts = p["alerts"] | true;
    String serverUrl = cfg["cric"]["url"] | "";
    if (serverUrl.endsWith("/api/cricket")) serverUrl = serverUrl.substring(0, serverUrl.length() - 12);
    if (!serverUrl.length()) return false;
    String url = serverUrl + "/api/football/all?league=" + league;

    if (team.length()) {
      time_t now = time(nullptr);
      struct tm a, b;
      time_t t1 = now - 4 * 86400, t2 = now + 8 * 86400;
      gmtime_r(&t1, &a);
      gmtime_r(&t2, &b);
      char q[40];
      snprintf(q, sizeof(q), "&date=%04d-%02d-%02d", a.tm_year + 1900, a.tm_mon + 1, a.tm_mday);
      url += q;
    }
    
    JsonDocument filter;
    JsonObject e = filter.add<JsonObject>();
    e["startTime"] = true;
    e["status"] = true;
    e["clock"] = true;
    e["home"]["short"] = true;
    e["home"]["name"] = true;
    e["home"]["score"] = true;
    e["away"]["short"] = true;
    e["away"]["name"] = true;
    e["away"]["score"] = true;
    JsonDocument doc;
    if (!httpJson(url, doc, &filter)) return false;

    FMatch all[40];
    int na = 0;
    time_t now = time(nullptr);
    for (JsonObject ev : doc.as<JsonArray>()) {
      if (na >= 40) break;
      FMatch m;
      memset(&m, 0, sizeof(m));
      const char* st = ev["status"] | "scheduled";
      m.state = !strcmp(st, "live") ? 1 : (!strcmp(st, "finished") ? 2 : 0);
      m.t = parseISO(ev["startTime"] | "");
      if (m.state == 1) safeCopy(m.clock, ev["clock"] | "", sizeof(m.clock));
      else safeCopy(m.clock, m.state == 2 ? "FT" : "", sizeof(m.clock));
      
      safeCopy(m.h, ev["home"]["short"] | "?", 6);
      safeCopy(m.hn, ev["home"]["name"] | "", 20);
      m.hs = ev["home"]["score"] | 0;

      safeCopy(m.a, ev["away"]["short"] | "?", 6);
      safeCopy(m.an, ev["away"]["name"] | "", 20);
      m.as = ev["away"]["score"] | 0;

      if (team.length() && !(icontains(m.hn, team.c_str()) || icontains(m.an, team.c_str()) ||
                             !strcasecmp(m.h, team.c_str()) || !strcasecmp(m.a, team.c_str())))
        continue;
      all[na++] = m;
    }
    FootData nd;"""
    content = re.sub(fetch_old, fetch_new, content, flags=re.DOTALL)

    # Update ftable_fetch
    ftable_old = r"""static bool ftable_fetch\(JsonObject p, void\* d\) \{.*?TableData td;"""
    ftable_new = """static bool ftable_fetch(JsonObject p, void* d) {
    String league = p["league"] | "eng.1";
    String serverUrl = cfg["cric"]["url"] | "";
    if (serverUrl.endsWith("/api/cricket")) serverUrl = serverUrl.substring(0, serverUrl.length() - 12);
    if (!serverUrl.length()) return false;
    
    JsonDocument filter;
    JsonObject e = filter.add<JsonObject>();
    e["rank"] = true;
    e["short"] = true;
    e["played"] = true;
    e["gd"] = true;
    e["points"] = true;
    JsonDocument doc;
    if (!httpJson(serverUrl + "/api/football/standings/" + league, doc, &filter)) return false;
    TableData td;"""
    content = re.sub(ftable_old, ftable_new, content, flags=re.DOTALL)
    
    # Update ftable processing
    ftable_proc_old = r"""memset\(&td, 0, sizeof\(td\)\);\n    for \(JsonObject e : doc\["children"\]\[0\]\["standings"\]\["entries"\].as<JsonArray>\(\)\) \{.*?td\.ok = td\.n > 0;"""
    ftable_proc_new = """memset(&td, 0, sizeof(td));
    for (JsonObject e : doc.as<JsonArray>()) {
      if (td.n >= 20) break;
      TRow& r = td.r[td.n++];
      safeCopy(r.t, e["short"] | "?", sizeof(r.t));
      r.rank = e["rank"] | 0;
      r.pl = e["played"] | 0;
      r.gd = e["gd"] | 0;
      r.pts = e["points"] | 0;
    }
    for (int i = 0; i < td.n; i++)
      for (int j = i + 1; j < td.n; j++)
        if (td.r[j].rank < td.r[i].rank) { TRow t = td.r[i]; td.r[i] = td.r[j]; td.r[j] = t; }
    td.ok = td.n > 0;"""
    content = re.sub(ftable_proc_old, ftable_proc_new, content, flags=re.DOTALL)

    with open(path, "w") as f:
        f.write(content)

def update_feeds():
    path = "EmoDesk/feeds.cpp"
    with open(path, "r") as f:
        content = f.read()

    # Update parseMatch
    parse_old = r"""static void parseMatch\(JsonObject m, CricMatch& c\) \{.*?\}\n  \}"""
    parse_new = """static void parseMatch(JsonObject m, CricMatch& c) {
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
  }"""
    content = re.sub(parse_old, parse_new, content, flags=re.DOTALL)

    # Update cricLoop
    cric_old = r"""    // the match server puts your team first when told about it.*?JsonArray arr = doc\["data"\];"""
    cric_new = """    if (url.endsWith("/api/cricket")) url = url.substring(0, url.length() - 12);
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
    JsonArray arr = doc.as<JsonArray>();"""
    content = re.sub(cric_old, cric_new, content, flags=re.DOTALL)
    
    # Update cricLoop processing (remove hitsToday logic since server doesn't provide it)
    hits_old = r"""    nd->hits = doc\["info"\]\["hitsToday"\].*?cricHits = max\(cricHits, nd->hits\);"""
    hits_new = """    nd->hits = 0;
    nd->limit = 0;"""
    content = re.sub(hits_old, hits_new, content, flags=re.DOTALL)
    
    # Update live condition to use server status
    live_old = r"""        bool live = \(m\["matchStarted"\] \| false\) && !\(m\["matchEnded"\] \| false\);"""
    live_new = """        const char* st = m["status"] | "";\n        bool live = !strcmp(st, "live");"""
    content = re.sub(live_old, live_new, content, flags=re.DOTALL)
    
    # Update mine condition to use server status
    mine_old = r"""bool mine = team\.length\(\) && icontains\(m\["name"\] \| "", team\.c_str\(\)\);"""
    mine_new = """bool mine = team.length() && icontains(m["title"] | "", team.c_str());"""
    content = re.sub(mine_old, mine_new, content, flags=re.DOTALL)

    with open(path, "w") as f:
        f.write(content)

update_cards_weather_sport()
update_feeds()
print("Patched!")
