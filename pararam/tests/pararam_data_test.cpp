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
			{ u"text"_q, u"Hi "_q },
			{ u"mention"_q, u"@bob"_q, QString(), 2 },
			{ u"text"_q, u", see "_q },
			{ u"link"_q, u"https://pararam.io"_q, u"https://pararam.io"_q },
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
			== u"**bold** `code` link (https://x.y)"_q);
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
	// Ids encoding used by /msg/post?ids=.
	CHECK(Api::EncodeDigit(0) == u"0"_q);
	CHECK(Api::EncodeDigit(64) == u"10"_q);
	CHECK(Api::EncodeDigit(63) == u"."_q);

	std::printf(Failures ? "%d FAILED\n" : "OK\n", Failures);
	return Failures ? 1 : 0;
}
