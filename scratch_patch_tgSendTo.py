path = "EmoDesk/feeds.cpp"
with open(path, "r") as f:
    content = f.read()

target = "void tgQueue(const char* text)"
insert_code = """static void tgSendTo(const String& tok, const String& chat, const char* text) {
  httpGet("https://api.telegram.org/bot" + tok + "/sendMessage?chat_id=" + chat + "&text=" + urlEncode(text), nullptr);
}

"""
if target in content and "static void tgSendTo" not in content:
    content = content.replace(target, insert_code + target)
    with open(path, "w") as f:
        f.write(content)
