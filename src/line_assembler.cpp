#include "line_assembler.hpp"

namespace atmodem {

namespace {
// Эхо терминатора - одиночный CR без LF. Реальные модемы по V.250
// возвращают "\r\n", но наш сервер добавляет CRLF-обрамление к каждому
// ответу (sendResponse), поэтому лишний LF дал бы пустую строку между
// эхом и ответом; интеграционные тесты закрепляют именно "\r".
constexpr std::string_view kCrEcho = "\r";      ///< Эхо терминатора команды.
}

LineAssembler::LineAssembler(std::size_t maxLineLength) noexcept
    : maxLineLength_(maxLineLength)
{
}

FeedResult LineAssembler::feed(char c)
{
    FeedResult result;

    // Строка уже завершена терминатором CR и ещё не забрана через
    // takeLine()/reset(): последующий ввод игнорируется, чтобы "AT\rZ\r"
    // не превращалось в "ATZ". Сервер забирает строку сразу после CR,
    // поэтому на него это не влияет.
    if (completed_) {
        return result;
    }

    switch (c) {
    case '\r': // терминатор AT-команды
        completed_ = true;
        result.lineCompleted = true;
        result.overflowed = overflow_;
        result.echo = true;
        result.echoSeq = kCrEcho;
        return result;

    case '\n': // прозрачно игнорируем LF (клиенты с CRLF)
    case '\0': // NUL-заполнители игнорируем
        return result;

    case 0x08: // BS
    case 0x7F: // DEL
        if (!buffer_.empty()) {
            buffer_.pop_back();
            if (buffer_.size() < maxLineLength_) {
                overflow_ = false; // после стирания строка снова в пределах лимита
            }
            result.echo = true;
            result.echoSeq = kEraseEcho;
        }
        return result;

    case 0x18: // CAN — отмена текущей командной строки
    case 0x15: // NAK — аналогично (некоторые терминалы шлют NAK)
        buffer_.clear();
        overflow_ = false;
        result.echo = true;
        result.echoSeq = kCancelEcho;
        return result;

    default:
        break;
    }

    if (static_cast<unsigned char>(c) < 0x20) {
        // Прочие управляющие символы молча отбрасываем.
        return result;
    }
    if (buffer_.size() >= maxLineLength_) {
        overflow_ = true;
        result.overflowed = true;
        // Реальный модем эхо-ит каждый байт, в т.ч. сверх лимита: символ
        // не накапливается, но отражается, как если бы был принят.
        result.echo = true;
        echoChar_[0] = c;
        result.echoSeq = std::string_view(echoChar_, 1);
        return result;
    }
    buffer_.push_back(c);
    result.echo = true;
    echoChar_[0] = c;
    result.echoSeq = std::string_view(echoChar_, 1);
    return result;
}

bool LineAssembler::takeLine(std::string& out)
{
    if (!completed_) {
        return false;
    }
    out = buffer_;
    buffer_.clear();
    completed_ = false;
    overflow_ = false; // строка забрана: флаг переполнения не должен "залипать"
    return true;
}

void LineAssembler::reset()
{
    // clear() сохраняет ёмкость: после сброса новые аллокации не нужны.
    buffer_.clear();
    completed_ = false;
    overflow_ = false;
}

} // namespace atmodem
