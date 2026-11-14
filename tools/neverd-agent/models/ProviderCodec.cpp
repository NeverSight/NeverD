//===- ProviderCodec.cpp - Model wire protocols ---------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Preserve signed/opaque reasoning while rendering only public answer text.
//===----------------------------------------------------------------------===//

#include "ProviderCodec.h"

#include <QJsonDocument>
#include <QSet>

namespace neverd::agent {
ProviderCodec::ProviderCodec(ModelProtocol protocol) : Protocol(protocol) {}

QJsonObject ProviderCodec::message(const QString &role, const QString &text) {
  return {{"role", role}, {"content", text}};
}

QJsonObject ProviderCodec::request(const ModelProfile &profile,
                                   const QString &system,
                                   const QJsonArray &history,
                                   const QJsonArray &tools) {
  QJsonObject result{{"model", profile.Model}, {"stream", true}};
  QJsonArray definitions;
  for (const auto &value : tools) {
    const auto tool = value.toObject();
    QJsonObject definition{{"name", tool["name"]},
                           {"description", tool["description"]}};
    if (profile.Protocol == ModelProtocol::Messages) {
      definition["input_schema"] = tool["input_schema"];
    } else {
      definition["parameters"] = tool["input_schema"];
      if (profile.Protocol == ModelProtocol::ChatCompletions)
        definition = {{"type", "function"}, {"function", definition}};
      else {
        definition["type"] = "function";
        definition["strict"] = false;
      }
    }
    definitions.append(definition);
  }
  if (!definitions.isEmpty())
    result["tools"] = definitions;
  if (!profile.ReasoningEffort.isEmpty()) {
    if (profile.Protocol == ModelProtocol::ChatCompletions)
      result["reasoning_effort"] = profile.ReasoningEffort;
    else
      result[profile.Protocol == ModelProtocol::Responses ? "reasoning"
                                                          : "output_config"] =
          QJsonObject{{"effort", profile.ReasoningEffort}};
  }
  if (profile.Protocol == ModelProtocol::Responses) {
    result["instructions"] = system;
    result["input"] = history;
    result["max_output_tokens"] = profile.OutputTokens;
    result["store"] = false;
    result["include"] = QJsonArray{"reasoning.encrypted_content"};
  } else if (profile.Protocol == ModelProtocol::Messages) {
    result["system"] = system;
    result["messages"] = history;
    result["max_tokens"] = profile.OutputTokens;
  } else {
    QJsonArray messages{message("system", system)};
    for (const auto &item : history)
      messages.append(item);
    result["messages"] = messages;
    result[profile.CompletionTokenField] = profile.OutputTokens;
    result["stream_options"] = QJsonObject{{"include_usage", true}};
  }
  return result;
}

void ProviderCodec::toolResults(ModelProtocol protocol, QJsonArray &history,
                                const QList<ModelToolCall> &calls,
                                const QList<QJsonObject> &results) {
  QJsonArray blocks;
  for (qsizetype i = 0; i < calls.size(); ++i) {
    const QString text = QString::fromUtf8(
        QJsonDocument(results[i]).toJson(QJsonDocument::Compact));
    if (protocol == ModelProtocol::Responses)
      history.append(QJsonObject{{"type", "function_call_output"},
                                 {"call_id", calls[i].Id},
                                 {"output", text}});
    else if (protocol == ModelProtocol::ChatCompletions)
      history.append(QJsonObject{
          {"role", "tool"}, {"tool_call_id", calls[i].Id}, {"content", text}});
    else
      blocks.append(QJsonObject{{"type", "tool_result"},
                                {"tool_use_id", calls[i].Id},
                                {"content", text},
                                {"is_error", results[i]["error"].isString()}});
  }
  if (!blocks.isEmpty())
    history.append(QJsonObject{{"role", "user"}, {"content", blocks}});
}

bool ProviderCodec::feed(const QByteArray &bytes,
                         const std::function<void(QString)> &delta) {
  Buffer += bytes;
  while (true) {
    const qsizetype end = Buffer.indexOf('\n');
    if (end < 0)
      break;
    QByteArray line = Buffer.left(end);
    Buffer.remove(0, end + 1);
    if (line.endsWith('\r'))
      line.chop(1);
    if (line.isEmpty()) {
      if (!EventData.isEmpty()) {
        EventData.chop(1);
        if (!event(EventData, delta))
          return false;
        EventData.clear();
      }
    } else if (line.startsWith("data:")) {
      line.remove(0, 5);
      if (line.startsWith(' '))
        line.remove(0, 1);
      EventData += line + '\n';
    }
    if (EventData.size() > model_limits::kMaxEventBytes) {
      Error = "Model stream event exceeds the configured size limit.";
      return false;
    }
  }
  if (Buffer.size() > model_limits::kMaxEventBytes) {
    Error = "Model stream line exceeds the configured size limit.";
    return false;
  }
  return Error.isEmpty();
}

bool ProviderCodec::event(const QByteArray &data,
                          const std::function<void(QString)> &delta) {
  if (data == "[DONE]") {
    if (Protocol != ModelProtocol::ChatCompletions || StopReason.isEmpty())
      Error = "Unexpected stream terminator.";
    else
      Completed = true;
    return Error.isEmpty();
  }
  QJsonParseError parse;
  const auto document = QJsonDocument::fromJson(data, &parse);
  if (parse.error != QJsonParseError::NoError || !document.isObject()) {
    Error = "Malformed model stream JSON.";
    return false;
  }
  const auto obj = document.object();
  const QString type = obj["type"].toString();
  if (obj.contains("error")) {
    Error =
        obj["error"].toObject()["message"].toString("Model returned an error.");
    return false;
  }
  if (Protocol == ModelProtocol::Responses) {
    if (type == "response.output_text.delta")
      delta(obj["delta"].toString());
    else if (type == "response.completed") {
      Final = obj["response"].toObject();
      Completed = true;
    } else if (type == "response.failed" || type == "response.incomplete") {
      Final = obj["response"].toObject();
      Usage = Final["usage"].toObject();
      const auto reason =
          Final["incomplete_details"].toObject()["reason"].toString();
      Error = Final["error"].toObject()["message"].toString(
          "Model response incomplete (" + reason +
          "); no tool calls were executed.");
    } else if (type == "error")
      Error = obj["message"].toString("Model stream error.");
  } else if (Protocol == ModelProtocol::ChatCompletions) {
    if (obj["usage"].isObject())
      Usage = obj["usage"].toObject();
    const auto choices = obj["choices"].toArray();
    if (choices.isEmpty())
      return true;
    const auto choice = choices[0].toObject();
    const auto d = choice["delta"].toObject();
    const auto text = d["content"].toString();
    Text += text;
    if (!text.isEmpty())
      delta(text);
    Reasoning += d["reasoning_content"].toString();
    for (const auto &value : d["tool_calls"].toArray()) {
      const auto call = value.toObject();
      const int index = call["index"].toInt(-1);
      if (index < 0) {
        Error = "Missing tool-call index.";
        return false;
      }
      auto &block = Blocks[index];
      for (const char *key : {"id", "type"})
        if (call.contains(key))
          block[key] = call[key];
      auto function = block["function"].toObject();
      const auto incoming = call["function"].toObject();
      for (const char *key : {"name", "arguments"})
        if (incoming[key].isString())
          function[key] = function[key].toString() + incoming[key].toString();
      block["function"] = function;
    }
    if (choice["finish_reason"].isString())
      StopReason = choice["finish_reason"].toString();
  } else {
    const int index = obj["index"].toInt(-1);
    if (type == "message_start")
      Usage = obj["message"].toObject()["usage"].toObject();
    else if (type == "content_block_start") {
      if (index < 0 || Blocks.contains(index)) {
        Error = "Invalid content block index.";
        return false;
      }
      Blocks[index] = obj["content_block"].toObject();
      if (Blocks[index]["type"] == "text")
        delta(Blocks[index]["text"].toString());
    } else if (type == "content_block_delta") {
      if (!Blocks.contains(index)) {
        Error = "Delta precedes content block.";
        return false;
      }
      const auto d = obj["delta"].toObject();
      auto &block = Blocks[index];
      const auto kind = d["type"].toString();
      if (kind == "text_delta") {
        block["text"] = block["text"].toString() + d["text"].toString();
        delta(d["text"].toString());
      } else if (kind == "input_json_delta")
        Arguments[index] += d["partial_json"].toString();
      else if (kind == "thinking_delta")
        block["thinking"] =
            block["thinking"].toString() + d["thinking"].toString();
      else if (kind == "signature_delta")
        block["signature"] =
            block["signature"].toString() + d["signature"].toString();
    } else if (type == "message_delta") {
      StopReason = obj["delta"].toObject()["stop_reason"].toString();
      const auto usage = obj["usage"].toObject();
      for (auto it = usage.begin(); it != usage.end(); ++it)
        Usage[it.key()] = it.value();
    } else if (type == "message_stop")
      Completed = true;
  }
  return Error.isEmpty();
}

ModelTurn ProviderCodec::finish(bool streaming, const QByteArray &body) {
  if (!Error.isEmpty()) {
    ModelTurn r;
    r.Error = Error;
    r.Usage = Usage;
    r.Output = Final["output"].toArray();
    for (const auto &item : r.Output)
      for (const auto &part : item.toObject()["content"].toArray())
        if (part.toObject()["type"] == "output_text")
          r.Text += part.toObject()["text"].toString();
    return r;
  }
  if (!streaming) {
    QJsonParseError parse;
    const auto doc = QJsonDocument::fromJson(body, &parse);
    if (parse.error != QJsonParseError::NoError || !doc.isObject()) {
      ModelTurn r;
      r.Error = "Malformed model response JSON.";
      return r;
    }
    return decode(doc.object());
  }
  if (!Completed) {
    ModelTurn r;
    r.Error = "Model stream ended without a completion event.";
    return r;
  }
  if (Protocol == ModelProtocol::Responses)
    return decode(Final);
  QJsonArray blocks;
  for (auto it = Blocks.begin(); it != Blocks.end(); ++it) {
    auto block = it.value();
    if (Protocol == ModelProtocol::Messages && Arguments.contains(it.key())) {
      const auto parsed = QJsonDocument::fromJson(Arguments[it.key()].toUtf8());
      if (!parsed.isObject()) {
        ModelTurn r;
        r.Error = "Malformed tool input JSON.";
        return r;
      }
      block["input"] = parsed.object();
    }
    blocks.append(block);
  }
  if (Protocol == ModelProtocol::Messages)
    return decode(
        {{"content", blocks}, {"stop_reason", StopReason}, {"usage", Usage}});
  QJsonObject message{{"role", "assistant"}, {"content", Text}};
  if (!Reasoning.isEmpty())
    message["reasoning_content"] = Reasoning;
  if (!blocks.isEmpty())
    message["tool_calls"] = blocks;
  return decode(
      {{"choices", QJsonArray{QJsonObject{{"message", message},
                                          {"finish_reason", StopReason}}}},
       {"usage", Usage}});
}

ModelTurn ProviderCodec::decode(const QJsonObject &object) {
  ModelTurn result;
  result.Usage = object["usage"].toObject();
  if (object.contains("error") && !object["error"].isNull()) {
    result.Error =
        object["error"].toObject()["message"].toString("Model request failed.");
    return result;
  }
  QJsonArray blocks;
  QString stop;
  if (Protocol == ModelProtocol::Responses) {
    stop = object["status"].toString();
    if (stop != "completed")
      result.Error = "Model response did not complete.";
    result.Output = object["output"].toArray();
    blocks = result.Output;
  } else if (Protocol == ModelProtocol::Messages) {
    stop = object["stop_reason"].toString();
    if (stop != "end_turn" && stop != "tool_use" && stop != "stop_sequence")
      result.Error = "Model stopped before completion: " + stop;
    blocks = object["content"].toArray();
    result.Output = {QJsonObject{{"role", "assistant"}, {"content", blocks}}};
  } else {
    const auto choices = object["choices"].toArray();
    if (choices.isEmpty()) {
      result.Error = "Missing model completion choice.";
      return result;
    }
    const auto choice = choices[0].toObject();
    stop = choice["finish_reason"].toString();
    if (stop != "stop" && stop != "tool_calls")
      result.Error = "Model stopped before completion: " + stop;
    const auto message = choice["message"].toObject();
    result.Text = message["content"].toString();
    result.Output = {message};
    blocks = message["tool_calls"].toArray();
  }
  if (!result.Error.isEmpty())
    return result;
  QSet<QString> ids;
  for (const auto &value : blocks) {
    const auto block = value.toObject();
    const QString type = block["type"].toString();
    if (type == "text")
      result.Text += block["text"].toString();
    if (type == "message")
      for (const auto &part : block["content"].toArray()) {
        if (part.toObject()["type"] == "output_text")
          result.Text += part.toObject()["text"].toString();
        if (part.toObject()["type"] == "refusal")
          result.Text += part.toObject()["refusal"].toString();
      }
    if (type != "function_call" && type != "tool_use" && type != "function")
      continue;
    ModelToolCall call;
    if (type == "tool_use") {
      call = {block["id"].toString(), block["name"].toString(),
              block["input"].toObject()};
      if (!block["input"].isObject())
        result.Error = "Tool input is not an object.";
    } else {
      const auto source =
          type == "function" ? block["function"].toObject() : block;
      const auto parsed =
          QJsonDocument::fromJson(source["arguments"].toString().toUtf8());
      if (!parsed.isObject())
        result.Error = "Malformed tool arguments; no tools were executed.";
      call = {block[type == "function" ? "id" : "call_id"].toString(),
              source["name"].toString(), parsed.object()};
    }
    if (call.Id.isEmpty() || call.Name.isEmpty() || ids.contains(call.Id))
      result.Error = "Missing or duplicate tool-call identity.";
    ids.insert(call.Id);
    result.Calls.append(call);
  }
  if ((stop == "tool_calls" || stop == "tool_use") && result.Calls.isEmpty())
    result.Error = "Tool stop reason has no tool calls.";
  if (!result.Error.isEmpty())
    result.Calls.clear();
  return result;
}
} // namespace neverd::agent
