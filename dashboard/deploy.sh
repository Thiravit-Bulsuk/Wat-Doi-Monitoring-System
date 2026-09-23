#!/usr/bin/env bash
set -euo pipefail

export PATH="/Applications/google-cloud-sdk/bin:${PATH}"

PROJECT_ID="${PROJECT_ID:-watdoi-monitoring}"
REGION="${REGION:-asia-southeast1}"
SERVICE_NAME="${SERVICE_NAME:-wat-doi-dashboard}"
MQTT_URL="${MQTT_URL:-mqtts://aa9e03df167b40b086069a385503cf2f.s1.eu.hivemq.cloud:8883}"
MQTT_USERNAME="${MQTT_USERNAME:-WebUser}"
MQTT_PASSWORD="${MQTT_PASSWORD:-12345678}"
APP_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if ! command -v gcloud >/dev/null 2>&1; then
  echo "Google Cloud SDK not found. Add /Applications/google-cloud-sdk/bin to PATH."
  exit 1
fi

echo "Using project: ${PROJECT_ID}"
echo "Using region: ${REGION}"
echo "Using service: ${SERVICE_NAME}"

gcloud config set project "${PROJECT_ID}"

if ! gcloud projects describe "${PROJECT_ID}" >/dev/null 2>&1; then
  echo "Project ${PROJECT_ID} does not exist. Create it first:"
  echo "gcloud projects create ${PROJECT_ID} --name=${PROJECT_ID} --set-as-default"
  exit 1
fi

gcloud services enable \
  run.googleapis.com \
  secretmanager.googleapis.com \
  cloudbuild.googleapis.com \
  artifactregistry.googleapis.com \
  --project "${PROJECT_ID}"

for secret_name in mqtt-username mqtt-password; do
  if ! gcloud secrets describe "${secret_name}" --project "${PROJECT_ID}" >/dev/null 2>&1; then
    case "${secret_name}" in
      mqtt-username)
        secret_value="${MQTT_USERNAME}"
        ;;
      mqtt-password)
        secret_value="${MQTT_PASSWORD}"
        ;;
      *)
        secret_value=""
        ;;
    esac

    if [[ -n "${secret_value}" ]]; then
      printf '%s' "${secret_value}" | gcloud secrets create "${secret_name}" \
        --project "${PROJECT_ID}" \
        --replication-policy=automatic \
        --data-file=-
    fi
  fi
done

PROJECT_NUMBER="$(gcloud projects describe "${PROJECT_ID}" --format='value(projectNumber)')"
SERVICE_ACCOUNT="${PROJECT_NUMBER}-compute@developer.gserviceaccount.com"

gcloud projects add-iam-policy-binding "${PROJECT_ID}" \
  --member="serviceAccount:${SERVICE_ACCOUNT}" \
  --role='roles/secretmanager.secretAccessor' \
  --quiet

gcloud run deploy "${SERVICE_NAME}" \
  --source "${APP_DIR}" \
  --region "${REGION}" \
  --project "${PROJECT_ID}" \
  --allow-unauthenticated \
  --set-env-vars="MQTT_URL=${MQTT_URL}" \
  --set-secrets="MQTT_USERNAME=mqtt-username:latest,MQTT_PASSWORD=mqtt-password:latest"

URL="$(gcloud run services describe "${SERVICE_NAME}" --region "${REGION}" --project "${PROJECT_ID}" --format='value(status.url)')"
echo
printf 'Deploy successful. URL:\n%s\n' "${URL}"
