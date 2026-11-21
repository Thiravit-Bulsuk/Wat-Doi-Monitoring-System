#!/usr/bin/env bash
set -euo pipefail

export PATH="/Applications/google-cloud-sdk/bin:${PATH}"
APP_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [[ -f "${APP_DIR}/.env" ]]; then
  set -a
  # shellcheck disable=SC1091
  source "${APP_DIR}/.env"
  set +a
fi

PROJECT_ID="${PROJECT_ID:-watdoi-monitoring}"
REGION="${REGION:-asia-southeast1}"
SERVICE_NAME="${SERVICE_NAME:-wat-doi-dashboard}"
MQTT_URL="${MQTT_URL:-mqtts://a7821161.ala.us-east-1.emqxsl.com:8883}"
MQTT_USERNAME="${MQTT_USERNAME:-WebUser}"
: "${MQTT_PASSWORD:?MQTT_PASSWORD is required in dashboard/.env (EMQX WebUser password)}"
: "${HISTORY_TOKEN:?HISTORY_TOKEN is required in dashboard/.env (same value as HISTORY_TOKEN in include/secrets.h)}"
: "${CONTROL_PASSWORD:?CONTROL_PASSWORD is required in dashboard/.env (password for the master command buttons)}"

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
  firestore.googleapis.com \
  --project "${PROJECT_ID}"

# History lives in the (default) Firestore database, the only one covered by the free tier.
if ! gcloud firestore databases describe --database='(default)' --project "${PROJECT_ID}" >/dev/null 2>&1; then
  gcloud firestore databases create --database='(default)' --location="${REGION}" \
    --type=firestore-native --project "${PROJECT_ID}"
fi
for group in waterLevels systemLogs; do
  gcloud firestore fields ttls update expireAt --collection-group="${group}" \
    --enable-ttl --async --project "${PROJECT_ID}" --quiet >/dev/null
done

# Adds a new version when the value changed and destroys the old ones:
# only 6 secret versions per billing account are free and disabled versions still count.
if ! gcloud secrets describe mqtt-password --project "${PROJECT_ID}" >/dev/null 2>&1; then
  printf '%s' "${MQTT_PASSWORD}" | gcloud secrets create mqtt-password \
    --project "${PROJECT_ID}" \
    --replication-policy=automatic \
    --data-file=-
elif [[ "$(gcloud secrets versions access latest --secret mqtt-password --project "${PROJECT_ID}")" != "${MQTT_PASSWORD}" ]]; then
  printf '%s' "${MQTT_PASSWORD}" | gcloud secrets versions add mqtt-password --project "${PROJECT_ID}" --data-file=-
  latest="$(gcloud secrets versions list mqtt-password --project "${PROJECT_ID}" --sort-by=~createTime --limit=1 --format='value(name)')"
  for old in $(gcloud secrets versions list mqtt-password --project "${PROJECT_ID}" --filter='state!=DESTROYED' --format='value(name)'); do
    [[ "${old}" == "${latest}" ]] || gcloud secrets versions destroy "${old}" --secret mqtt-password --project "${PROJECT_ID}" --quiet >/dev/null
  done
fi

PROJECT_NUMBER="$(gcloud projects describe "${PROJECT_ID}" --format='value(projectNumber)')"
SERVICE_ACCOUNT="${PROJECT_NUMBER}-compute@developer.gserviceaccount.com"

gcloud projects add-iam-policy-binding "${PROJECT_ID}" \
  --member="serviceAccount:${SERVICE_ACCOUNT}" \
  --role='roles/secretmanager.secretAccessor' \
  --quiet

gcloud projects add-iam-policy-binding "${PROJECT_ID}" \
  --member="serviceAccount:${SERVICE_ACCOUNT}" \
  --role='roles/datastore.user' \
  --condition=None \
  --quiet >/dev/null

# One instance, CPU only while serving requests: keeps Cloud Run and Firestore reads in the free tier.
# HISTORY_TOKEN is a plain env var (not Secret Manager) because the free 6 secret versions are already used.
gcloud run deploy "${SERVICE_NAME}" \
  --source "${APP_DIR}" \
  --region "${REGION}" \
  --project "${PROJECT_ID}" \
  --allow-unauthenticated \
  --min-instances=0 --max-instances=1 --cpu-throttling \
  --set-env-vars="^@^MQTT_URL=${MQTT_URL}@MQTT_USERNAME=${MQTT_USERNAME}@HISTORY_TOKEN=${HISTORY_TOKEN}@CONTROL_PASSWORD=${CONTROL_PASSWORD}" \
  --set-secrets="MQTT_PASSWORD=mqtt-password:latest"

URL="$(gcloud run services describe "${SERVICE_NAME}" --region "${REGION}" --project "${PROJECT_ID}" --format='value(status.url)')"
echo
printf 'Deploy successful. URL:\n%s\n' "${URL}"
