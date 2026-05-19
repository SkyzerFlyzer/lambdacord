variable "aws_region" {
  description = "AWS region for all Lambda functions."
  type        = string
  default     = "us-east-1"
}

variable "project_name" {
  description = "Prefix used for IAM resources and optional tags."
  type        = string
  default     = "lambdacord-bot"
}

variable "discord_public_key" {
  description = "Discord application public key used to verify interaction signatures."
  type        = string
  sensitive   = true
}

variable "lambda_architecture" {
  description = "Lambda architecture. Must match the built zip artifacts."
  type        = string
  default     = "arm64"

  validation {
    condition     = contains(["arm64", "x86_64"], var.lambda_architecture)
    error_message = "lambda_architecture must be arm64 or x86_64."
  }
}

variable "lambda_runtime" {
  description = "Custom runtime used by the packaged C++ Lambda functions."
  type        = string
  default     = "provided.al2023"
}

variable "router_lambda_memory_size" {
  description = "Memory size in MB for ingress and routing Lambdas."
  type        = number
  default     = 256
}

variable "router_lambda_timeout" {
  description = "Timeout in seconds for ingress and routing Lambdas."
  type        = number
  default     = 10
}

variable "worker_lambda_memory_size" {
  description = "Memory size in MB for downstream worker Lambdas."
  type        = number
  default     = 256
}

variable "worker_lambda_timeout" {
  description = "Timeout in seconds for downstream worker Lambdas."
  type        = number
  default     = 30
}

variable "tags" {
  description = "Tags applied to supported resources."
  type        = map(string)
  default     = {}
}
