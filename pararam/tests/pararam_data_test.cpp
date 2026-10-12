// Standalone tests for pararam/pararam_data.cpp, see pararam/tests/run.sh.
#include "pararam/pararam_data.h"
#include "pararam/pararam_api.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QJsonDocument>

#include <cstdio>

namespace {

int Failures = 0;

void Check(bool condition, const char *what, int line) {
	if (!condition) {
		std::printf("FAIL line %d: %s\n", line, what);
		++Failures;
	}
}

#define CHECK(x) Check((x), #x, __LINE__)

QJsonObject Json(const char *text) {
	return QJsonDocument::fromJson(QByteArray(text)).object();
}

} // namespace

int main(int argc, char *argv[]) {
	QCoreApplication app(argc, argv);
	using namespace Pararam;

	// Markdown and parsed parts to entities.
	{
		auto parsed = std::vector<TextPart>{
			{ .type = u"text"_q, .value = u"Hi "_q },
			{ .type = u"mention"_q, .value = u"@bob"_q, .userId = 2 },
			{ .type = u"text"_q, .value = u", see "_q },
			{
				.type = u"link"_q,
				.value = u"https://pararam.io"_q,
				.link = u"https://pararam.io"_q,
			},
			{ .type = u"text"_q, .value = u" "_q },
			{ .type = u"bold"_q, .value = u"now"_q },
			{ .type = u"text"_q, .value = u" "_q },
			{ .type = u"blockquote2"_q, .value = u"x"_q },
		};
		const auto result = ParseIncomingText(
			u"Hi @bob, see https://pararam.io **now** `x`"_q,
			parsed);
		CHECK(result.text == u"Hi @bob, see https://pararam.io now x"_q);
		CHECK(result.entities.size() == 4);
		CHECK(result.entities[0].type == Entity::Type::MentionName);
		CHECK(result.entities[0].offset == 3 && result.entities[0].length == 4);
		CHECK(result.entities[0].userId == 2);
		CHECK(result.entities[1].type == Entity::Type::Url);
		CHECK(result.entities[2].type == Entity::Type::Bold);
		CHECK(result.text.mid(result.entities[2].offset, result.entities[2].length) == u"now"_q);
		CHECK(result.entities[3].type == Entity::Type::Code);
	}
	{
		// Markdown links come as link parts with a title.
		auto parsed = std::vector<TextPart>{
			{ .type = u"text"_q, .value = u"Notes are in "_q },
			{
				.type = u"link"_q,
				.value = u"[the notes](https://x.y/n)"_q,
				.link = u"https://x.y/n"_q,
				.title = u"the notes"_q,
			},
			{ .type = u"br"_q },
			{ .type = u"strike"_q, .value = u"old"_q },
			{ .type = u"tag"_q, .value = u"#tag"_q },
		};
		const auto result = ParseIncomingText(
			u"Notes are in [the notes](https://x.y/n)\n~~old~~#tag"_q,
			parsed);
		CHECK(result.text == u"Notes are in the notes\nold#tag"_q);
		CHECK(result.entities.size() == 3);
		CHECK(result.entities[0].type == Entity::Type::TextUrl);
		CHECK(result.entities[0].url == u"https://x.y/n"_q);
		CHECK(result.text.mid(result.entities[0].offset, result.entities[0].length) == u"the notes"_q);
		CHECK(result.entities[1].type == Entity::Type::Strike);
		CHECK(result.entities[2].type == Entity::Type::Hashtag);
	}
	{
		// Code blocks with a language.
		auto parsed = std::vector<TextPart>{
			{ .type = u"blockquote1"_q, .value = u"cpp\nint x;\n"_q, .lang = u"cpp"_q },
		};
		const auto result = ParseIncomingText(u"```cpp\nint x;\n```"_q, parsed);
		CHECK(result.text == u"int x;"_q);
		CHECK(result.entities.size() == 1
			&& result.entities[0].type == Entity::Type::Pre
			&& result.entities[0].url == u"cpp"_q);
	}
	{
		const auto result = ParseIncomingText(
			u"before\n```cpp\nint x;\n```\nafter"_q,
			{});
		CHECK(result.text == u"before\nint x;\nafter"_q);
		CHECK(result.entities.size() == 1);
		CHECK(result.entities[0].type == Entity::Type::Pre);
		CHECK(result.entities[0].url == u"cpp"_q);
	}
	{
		// Unclosed markers stay as text.
		const auto result = ParseIncomingText(u"2 ** 3 and `x"_q, {});
		CHECK(result.text == u"2 ** 3 and `x"_q);
		CHECK(result.entities.empty());
	}
	// Entities to markdown.
	{
		auto text = FormattedText{ u"bold code link"_q };
		text.entities.push_back({ Entity::Type::Bold, 0, 4 });
		text.entities.push_back({ Entity::Type::Code, 5, 4 });
		text.entities.push_back({
			.type = Entity::Type::TextUrl,
			.offset = 10,
			.length = 4,
			.url = u"https://x.y"_q,
		});
		CHECK(FormatOutgoingText(text)
			== u"**bold** `code` [link](https://x.y)"_q);
	}
	{
		auto text = FormattedText{ u"ab"_q };
		text.entities.push_back({ Entity::Type::Bold, 0, 2 });
		text.entities.push_back({ Entity::Type::Strike, 0, 1 });
		CHECK(FormatOutgoingText(text) == u"**~~a~~b**"_q);
	}
	// Round trip.
	{
		auto text = FormattedText{ u"x bold y"_q };
		text.entities.push_back({ Entity::Type::Bold, 2, 4 });
		const auto back = ParseIncomingText(FormatOutgoingText(text), {});
		CHECK(back.text == text.text);
		CHECK(back.entities.size() == 1 && back.entities[0].offset == 2);
	}
	// HTML-escaped text from pararam.io is unescaped once, like the web.
	CHECK(UnescapeHtml(u"&lt;br /&gt;\n&lt;b&gt;Warning&lt;/b&gt;: a &amp;&amp; b"_q)
		== u"<br />\n<b>Warning</b>: a && b"_q);
	CHECK(UnescapeHtml(u"&amp;lt; &#x27;q&#x27; &#x2F; &#123;&#125; &copy;"_q)
		== u"&lt; 'q' / {} &copy;"_q);
	{
		const auto post = PostInfo::Parse(Json(R"({
			"post_no": 7, "chat_id": 10, "user_id": 3,
			"text": "```\n&lt;b&gt;Warning&lt;/b&gt;\n```",
			"text_parsed": [{"type": "blockquote1",
				"value": "\n&lt;b&gt;Warning&lt;/b&gt;\n"}]
		})"));
		const auto result = ParseIncomingText(post.text, post.parsed);
		CHECK(result.text == u"<b>Warning</b>"_q);
		CHECK(result.entities.size() == 1
			&& result.entities[0].type == Entity::Type::Pre);
	}
	// Dates.
	CHECK(ParseTime(u"1970-01-01T00:01:00.123456Z"_q) == 60);
	CHECK(ParseTime(u"2024-01-01T00:00:00Z"_q) == 1704067200);
	CHECK(ParseTime(QString()) == 0);
	// Parsing.
	{
		const auto chat = ChatInfo::Parse(Json(R"({
			"id": 10, "type": "group", "title": "General",
			"posts_count": 125, "last_read_post_no": "100",
			"thread_users": [1, 2, 3], "thread_admins": [1],
			"is_favorite": true, "tshow": false, "mute": "total",
			"time_updated": "2024-01-01T00:00:00.000000Z"
		})"));
		CHECK(chat.id == 10 && !chat.pm && chat.postsCount == 125);
		CHECK(chat.lastReadPostNo == 100);
		CHECK(chat.users.size() == 3 && chat.admins.size() == 1);
		CHECK(chat.favorite && !chat.visible && chat.muted());
		CHECK(chat.updated == 1704067200);
	}
	{
		const auto post = PostInfo::Parse(Json(R"({
			"post_no": 4, "thread_id": 20, "user_id": 3, "text": "hi",
			"reply_no": 2, "is_deleted": false, "ver": 3,
			"meta": {
				"user": {"id": 3, "name": "Carol", "unique_name": "carol"},
				"file": {"guid": "g1", "name": "a.png", "size": 10,
					"mime_type": "image/png"},
				"attachments": ["g1"]
			},
			"event": {"type": "CHAT_TITLE", "data": {"title": "New"}}
		})"));
		CHECK(post.chatId == 20 && post.postNo == 4 && post.replyNo == 2);
		CHECK(post.metaUser.name == u"Carol"_q);
		CHECK(post.file.valid() && post.file.isImage());
		CHECK(post.attachments.size() == 1);
		CHECK(post.isService() && post.eventType == u"CHAT_TITLE"_q);
	}
	// Reactions, one per user.
	{
		const auto post = PostInfo::Parse(Json(R"({
			"post_no": 5, "chat_id": 10, "user_id": 3, "text": "x",
			"meta": {"reactions": {"\ud83d\udd25": [4], "\ud83d\udc4d": [3, 4]}}
		})"));
		auto reactions = post.reactions;
		CHECK(reactions.list.size() == 2);
		CHECK(!reactions.list.empty() && reactions.list[0].first == u"\U0001F44D"_q);
		CHECK(reactions.count() == 3);
		CHECK(reactions.of(3) == u"\U0001F44D"_q);
		reactions.set(3, u"\U0001F525"_q);
		CHECK(reactions.of(3) == u"\U0001F525"_q);
		CHECK(reactions.count() == 3);
		reactions.set(4, QString());
		CHECK(reactions.list.size() == 1 && reactions.count() == 1);
		CHECK(!(reactions == post.reactions));
		CHECK(ReactionKinds().size() == 47);
		CHECK(ReactionIndex(u"\u2764"_q) >= 0);
		// Standard emoji in the client, pararam.io characters on the server.
		CHECK(ReactionIndex(u"\u2764\uFE0F"_q) == ReactionIndex(u"\u2764"_q));
		CHECK(StandardReaction(u"\u2714"_q) == u"\u2714\uFE0F"_q);
		CHECK(StandardReaction(u"\u2764"_q) == u"\u2764\uFE0F"_q);
		CHECK(StandardReaction(u"\U0001F44D"_q) == u"\U0001F44D"_q);
		CHECK(PararamReaction(u"\u2714\uFE0F"_q) == u"\u2714"_q);
		CHECK(PararamReaction(u"\u26A0"_q) == u"\u26A0\uFE0F"_q);
		const auto both = Reactions::Parse(QJsonValue(Json(R"({
			"\u2764": [3], "\u2764\ufe0f": [4]})")));
		CHECK(both.list.size() == 1 && both.count() == 2);
		CHECK(!both.list.empty() && both.list[0].first == u"\u2764"_q);
	}
	// Image types by name, HEIC needs the server side conversion.
	{
		const auto heic = FileInfo::Parse(Json(R"({
			"guid": "g", "name": "IMG_1.HEIC", "size": 5})"));
		CHECK(heic.isImage() && heic.needsConversion());
		const auto jpeg = FileInfo::Parse(Json(R"({
			"guid": "g", "name": "photo.JPG", "size": 5})"));
		CHECK(jpeg.mimeType == u"image/jpeg"_q && jpeg.isImage());
		const auto text = FileInfo::Parse(Json(R"({
			"guid": "g", "name": "a.txt", "size": 5})"));
		CHECK(!text.isImage());
	}
	// Ids encoding used by /msg/post?ids=.
	CHECK(Api::EncodeDigit(0) == u"0"_q);
	CHECK(Api::EncodeDigit(64) == u"10"_q);
	CHECK(Api::EncodeDigit(63) == u"."_q);

	std::printf(Failures ? "%d FAILED\n" : "OK\n", Failures);
	return Failures ? 1 : 0;
}
