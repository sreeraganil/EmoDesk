import re

path = "EmoDesk/cards_weather_sport.cpp"
with open(path, "r") as f:
    content = f.read()

ftable_proc_old = r"""memset\(&td, 0, sizeof\(td\)\);\s*for \(JsonObject e : doc\["children"\]\[0\]\["standings"\]\["entries"\].as<JsonArray>\(\)\) \{.*?td\.ok = td\.n > 0;"""
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

new_content = re.sub(ftable_proc_old, ftable_proc_new, content, flags=re.DOTALL)

with open(path, "w") as f:
    f.write(new_content)
