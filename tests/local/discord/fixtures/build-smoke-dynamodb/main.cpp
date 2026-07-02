// Build-smoke fixture for task T5.1.
//
// This is NOT a deployed Lambda. It exists only to prove that the builder
// image ships the aws-sdk-cpp DynamoDB client and that the auto-generated
// CMakeLists links it, so command Lambdas can construct a DynamoDBClient.
// The harness/agent builds it with scripts/build-lambda.sh; it is never part
// of scripts/build-all-lambdas.sh and never packaged for deployment.

#include <iostream>

#include <aws/core/Aws.h>
#include <aws/core/client/ClientConfiguration.h>
#include <aws/dynamodb/DynamoDBClient.h>

int main() {
  Aws::SDKOptions options{};
  Aws::InitAPI(options);
  {
    Aws::Client::ClientConfiguration config{};
    Aws::DynamoDB::DynamoDBClient dynamodb_client{config};
    (void)dynamodb_client;
    std::cout << "build-smoke-dynamodb: DynamoDBClient constructed" << std::endl;
  }
  Aws::ShutdownAPI(options);
  return 0;
}
