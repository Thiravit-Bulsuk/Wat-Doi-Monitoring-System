# Wat Doi Dashboard Deployment

This project deploys the dashboard to Google Cloud Run.

## Project

- Project ID: `watdoi-monitoring`
- Region: `asia-southeast1`
- Service name: `wat-doi-dashboard`

## Requirements

- Google Cloud SDK installed at `/Applications/google-cloud-sdk/bin`
- Billing enabled for the project
- Access to the Google Cloud account used for deployment

## First-time setup

1. Create the project in GCP:

   ```bash
   /Applications/google-cloud-sdk/bin/gcloud projects create watdoi-monitoring --name=watdoi-monitoring --set-as-default
   ```

2. Enable billing for the project in Google Cloud Console.

3. Run the deploy script:

   ```bash
   cd /Users/koy/Documents/PlatformIO/Projects/Wat-Doi-Monitoring-System/dashboard
   chmod +x deploy.sh
   ./deploy.sh
   ```

4. The script will:
   - set the project
   - enable required APIs
   - create the MQTT secrets if missing
   - grant Cloud Run access to read the secrets
   - deploy the app to Cloud Run and print the service URL

## Redeploy later

```bash
cd /Users/koy/Documents/PlatformIO/Projects/Wat-Doi-Monitoring-System/dashboard
PROJECT_ID=watdoi-monitoring ./deploy.sh
```

## Manual deploy command

```bash
cd /Users/koy/Documents/PlatformIO/Projects/Wat-Doi-Monitoring-System/dashboard

/Applications/google-cloud-sdk/bin/gcloud run deploy wat-doi-dashboard \
  --source . \
  --region=asia-southeast1 \
  --project watdoi-monitoring \
  --allow-unauthenticated \
  --set-env-vars="MQTT_URL=mqtts://aa9e03df167b40b086069a385503cf2f.s1.eu.hivemq.cloud:8883" \
  --set-secrets="MQTT_USERNAME=mqtt-username:latest,MQTT_PASSWORD=mqtt-password:latest"
```

## Cleanup old deployment

Only after the new project is confirmed to work:

```bash
/Applications/google-cloud-sdk/bin/gcloud run services delete wat-doi-dashboard \
  --region=asia-southeast1 \
  --project dji-flight-parser \
  --quiet
```

## Service URL

```bash
/Applications/google-cloud-sdk/bin/gcloud run services describe wat-doi-dashboard \
  --region=asia-southeast1 \
  --project watdoi-monitoring \
  --format='value(status.url)'
```
