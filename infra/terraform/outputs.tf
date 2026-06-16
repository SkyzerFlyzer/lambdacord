output "discord_function_url" {
  description = "Public Lambda Function URL to configure as the Discord interactions endpoint."
  value       = aws_lambda_function_url.discord_interactions.function_url
}

output "router_lambda_names" {
  description = "Names of the ingress and routing Lambda functions."
  value       = sort(keys(aws_lambda_function.router))
}

output "discord_interaction_dedup_table_name" {
  description = "DynamoDB table used by the Discord interactions framework for interaction idempotency."
  value       = aws_dynamodb_table.discord_interaction_dedup.name
}

output "discord_interaction_dedup_table_arn" {
  description = "ARN of the Discord interaction idempotency DynamoDB table."
  value       = aws_dynamodb_table.discord_interaction_dedup.arn
}
