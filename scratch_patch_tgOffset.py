path = "EmoDesk/feeds.cpp"
with open(path, "r") as f:
    content = f.read()

target = "static void tgSendTo"
insert_code = "static int64_t tgOffset = 0;\n"

if target in content and "tgOffset = 0" not in content:
    content = content.replace(target, insert_code + target)
    with open(path, "w") as f:
        f.write(content)
