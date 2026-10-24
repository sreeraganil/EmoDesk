path = "EmoDesk/cards_weather_sport.cpp"
with open(path, "r") as f:
    content = f.read()

start = content.find("static bool ftable_fetch(JsonObject p, void* d) {")
end = content.find("*(TableData*)d = td;", start) + 21
end = content.find("}", end) + 1

new_code = """static bool ftable_fetch(JsonObject p, void* d) {
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
  TableData td;
  memset(&td, 0, sizeof(td));
  for (JsonObject ev : doc.as<JsonArray>()) {
    if (td.n >= 20) break;
    TRow& r = td.r[td.n++];
    safeCopy(r.t, ev["short"] | "?", sizeof(r.t));
    r.rank = ev["rank"] | 0;
    r.pl = ev["played"] | 0;
    r.gd = ev["gd"] | 0;
    r.pts = ev["points"] | 0;
  }
  for (int i = 0; i < td.n; i++)
    for (int j = i + 1; j < td.n; j++)
      if (td.r[j].rank < td.r[i].rank) { TRow t = td.r[i]; td.r[i] = td.r[j]; td.r[j] = t; }
  td.ok = td.n > 0;
  *(TableData*)d = td;
  return td.ok;
}"""

content = content[:start] + new_code + content[end:]
with open(path, "w") as f:
    f.write(content)
