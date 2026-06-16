#pragma once

#include <aws/dynamodb/DynamoDBClient.h>
#include <aws/dynamodb/model/AttributeValue.h>
#include <aws/dynamodb/model/UpdateItemRequest.h>

#include <stdexcept>
#include <string>

namespace discord_interactions {

inline void dedup_mark(Aws::DynamoDB::DynamoDBClient& db,
                       const std::string& table,
                       const std::string& interaction_id,
                       const std::string& status) {
    if (table.empty() || interaction_id.empty()) {
        return;
    }
    if (status != "done" && status != "failed") {
        throw std::runtime_error("invalid Discord interaction dedup status");
    }

    Aws::DynamoDB::Model::AttributeValue pk{};
    pk.SetS(interaction_id);

    Aws::DynamoDB::Model::AttributeValue status_value{};
    status_value.SetS(status);

    Aws::DynamoDB::Model::UpdateItemRequest request{};
    request.SetTableName(table);
    request.AddKey("pk", pk);
    request.SetUpdateExpression("SET #status = :status");
    request.AddExpressionAttributeNames("#status", "status");
    request.AddExpressionAttributeValues(":status", status_value);

    const auto outcome = db.UpdateItem(request);
    if (!outcome.IsSuccess()) {
        throw std::runtime_error("failed to update Discord interaction dedup status: " +
                                 outcome.GetError().GetMessage());
    }
}

}  // namespace discord_interactions
