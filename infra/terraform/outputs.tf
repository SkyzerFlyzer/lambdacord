output "discord_function_url" {
  description = "Public Lambda Function URL to configure as the Discord interactions endpoint."
  value       = aws_lambda_function_url.discord_interactions.function_url
}

output "router_lambda_names" {
  description = "Names of the ingress and routing Lambda functions."
  value       = sort(keys(aws_lambda_function.router))
}

output "discord_idempotency_table_name" {
  description = "Name of the optional durable interaction-dedup DynamoDB table, or null when disabled. Feed into worker DISCORD_IDEMPOTENCY_TABLE and module IAM."
  value       = one(aws_dynamodb_table.discord_idempotency[*].name)
}

output "discord_idempotency_table_arn" {
  description = "ARN of the optional durable interaction-dedup DynamoDB table, or null when disabled. Attach to worker IAM to grant GetItem/PutItem."
  value       = one(aws_dynamodb_table.discord_idempotency[*].arn)
}
