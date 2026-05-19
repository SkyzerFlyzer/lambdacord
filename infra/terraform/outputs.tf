output "discord_function_url" {
  description = "Public Lambda Function URL to configure as the Discord interactions endpoint."
  value       = aws_lambda_function_url.discord_interactions.function_url
}

output "router_lambda_names" {
  description = "Names of the ingress and routing Lambda functions."
  value       = sort(keys(aws_lambda_function.router))
}
