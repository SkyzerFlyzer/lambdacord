data "aws_caller_identity" "current" {}

data "aws_partition" "current" {}

locals {
  common_tags = merge(
    {
      Project   = var.project_name
      ManagedBy = "Terraform"
    },
    var.tags
  )

  zip_root = abspath("${path.module}/../../packaged-lambdas")

  discord_command_routes = merge([
    for manifest in local.module_manifests : manifest.routes.commands
  ]...)

  discord_component_routes = merge([
    for manifest in local.module_manifests : manifest.routes.components
  ]...)

  router_functions = {
    "discord-interactions" = {
      zip_path = "${local.zip_root}/discord-interactions.zip"
      environment = {
        DISCORD_PUBLIC_KEY = var.discord_public_key
      }
    }
    "discord-application-command-handler" = {
      zip_path = "${local.zip_root}/discord-application-command-handler.zip"
      environment = {
        DISCORD_COMMAND_ROUTES = jsonencode(local.discord_command_routes)
      }
    }
    "discord-message-component-handler" = {
      zip_path = "${local.zip_root}/discord-message-component-handler.zip"
      environment = {
        DISCORD_COMPONENT_ROUTES = jsonencode(local.discord_component_routes)
      }
    }
    "discord-modal-handler" = {
      zip_path    = "${local.zip_root}/discord-modal-handler.zip"
      environment = {}
    }
    "discord-autocomplete-handler" = {
      zip_path    = "${local.zip_root}/discord-autocomplete-handler.zip"
      environment = {}
    }
  }

  router_invoke_targets = [
    "discord-application-command-handler",
    "discord-message-component-handler",
    "discord-modal-handler",
    "discord-autocomplete-handler",
    "discord-cmd-*",
    "discord-component-*",
    "discord-modal-*",
    "discord-autocomplete-*",
    "discord-error-mapper-*",
  ]

  router_invoke_arns = [
    for function_name in local.router_invoke_targets :
    "arn:${data.aws_partition.current.partition}:lambda:${var.aws_region}:${data.aws_caller_identity.current.account_id}:function:${function_name}"
  ]
}

data "aws_iam_policy_document" "lambda_assume_role" {
  statement {
    effect = "Allow"

    principals {
      type        = "Service"
      identifiers = ["lambda.amazonaws.com"]
    }

    actions = ["sts:AssumeRole"]
  }
}

resource "aws_iam_role" "router_lambda" {
  name               = "${var.project_name}-router-lambda-role"
  assume_role_policy = data.aws_iam_policy_document.lambda_assume_role.json
  tags               = local.common_tags
}

resource "aws_iam_role_policy_attachment" "router_basic_execution" {
  role       = aws_iam_role.router_lambda.name
  policy_arn = "arn:${data.aws_partition.current.partition}:iam::aws:policy/service-role/AWSLambdaBasicExecutionRole"
}

data "aws_iam_policy_document" "router_lambda_invoke" {
  statement {
    effect = "Allow"
    actions = [
      "lambda:InvokeFunction",
    ]
    resources = local.router_invoke_arns
  }
}

resource "aws_iam_role_policy" "router_lambda_invoke" {
  name   = "${var.project_name}-router-lambda-invoke"
  role   = aws_iam_role.router_lambda.id
  policy = data.aws_iam_policy_document.router_lambda_invoke.json
}

resource "aws_lambda_function" "router" {
  for_each = local.router_functions

  function_name    = each.key
  role             = aws_iam_role.router_lambda.arn
  filename         = each.value.zip_path
  source_code_hash = filebase64sha256(each.value.zip_path)
  runtime          = var.lambda_runtime
  handler          = "bootstrap"
  architectures    = [var.lambda_architecture]
  memory_size      = var.router_lambda_memory_size
  timeout          = var.router_lambda_timeout

  environment {
    variables = each.value.environment
  }

  tags = local.common_tags

  depends_on = [
    aws_iam_role_policy_attachment.router_basic_execution,
    aws_iam_role_policy.router_lambda_invoke,
  ]
}

resource "aws_lambda_function_url" "discord_interactions" {
  function_name      = aws_lambda_function.router["discord-interactions"].function_name
  authorization_type = "NONE"

  cors {
    allow_credentials = false
    allow_methods     = ["POST"]
    allow_origins     = ["*"]
    allow_headers = [
      "content-type",
      "x-signature-ed25519",
      "x-signature-timestamp",
    ]
    expose_headers = []
    max_age        = 0
  }
}

resource "aws_lambda_permission" "discord_interactions_function_url" {
  statement_id           = "AllowPublicInvokeFromFunctionUrl"
  action                 = "lambda:InvokeFunctionUrl"
  function_name          = aws_lambda_function.router["discord-interactions"].function_name
  principal              = "*"
  function_url_auth_type = "NONE"
}

resource "terraform_data" "discord_interactions_function_url_invoke_permission" {
  input = {
    function_name = aws_lambda_function.router["discord-interactions"].function_name
    region        = var.aws_region
    statement_id  = "AllowPublicInvokeFunctionFromFunctionUrl"
  }

  triggers_replace = [
    aws_lambda_function.router["discord-interactions"].function_name,
  ]

  provisioner "local-exec" {
    command = <<-EOT
      aws lambda remove-permission \
        --region '${self.input.region}' \
        --function-name '${self.input.function_name}' \
        --statement-id '${self.input.statement_id}' >/dev/null 2>&1 || true

      aws lambda add-permission \
        --region '${self.input.region}' \
        --function-name '${self.input.function_name}' \
        --statement-id '${self.input.statement_id}' \
        --action lambda:InvokeFunction \
        --principal '*' \
        --invoked-via-function-url >/dev/null
    EOT
  }

  provisioner "local-exec" {
    when    = destroy
    command = <<-EOT
      aws lambda remove-permission \
        --region '${self.input.region}' \
        --function-name '${self.input.function_name}' \
        --statement-id '${self.input.statement_id}' >/dev/null 2>&1 || true
    EOT
  }

  depends_on = [
    aws_lambda_function_url.discord_interactions,
  ]
}

# Optional durable interaction-dedup table (Phase 5 / AD-9). Disabled by default;
# worker Lambdas opt in via the DISCORD_IDEMPOTENCY_TABLE env var. On-demand
# billing, a single string hash key, and a TTL attribute so DynamoDB expires old
# completion/claim records automatically.
resource "aws_dynamodb_table" "discord_idempotency" {
  count = var.discord_idempotency_table_enabled ? 1 : 0

  name         = "${var.project_name}-discord-idempotency"
  billing_mode = "PAY_PER_REQUEST"
  hash_key     = "interaction_id"

  attribute {
    name = "interaction_id"
    type = "S"
  }

  ttl {
    attribute_name = "expires_at"
    enabled        = true
  }

  tags = local.common_tags
}
