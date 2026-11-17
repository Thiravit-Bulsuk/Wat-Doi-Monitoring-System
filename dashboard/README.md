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

## Do not run `gcloud run deploy` by hand

Always deploy with `deploy.sh`. `gcloud run deploy --set-env-vars` replaces every env var on the service, so a manual command without `HISTORY_TOKEN` and `CONTROL_PASSWORD` removes them: history uploads then return 503 and the master command buttons stop working. `deploy.sh` sets all of them from `dashboard/.env`.

## Service URL

```bash
/Applications/google-cloud-sdk/bin/gcloud run services describe wat-doi-dashboard \
  --region=asia-southeast1 \
  --project watdoi-monitoring \
  --format='value(status.url)'
```
