# blikk-attest-check

Litet CLI-program i C som loggar in mot [Blikks publika REST-API](https://publicapidocs.blikk.com/)
och kontrollerar attestflaggorna för innevarande månads tidrapporter. Byggt för
att köras som ett steg i ett automationsflöde (Power Automate/Copilot Studio,
Google Apps Script + Cloud Scheduler, cron, ...) — det skriver en
maskinläsbar JSON-summering på stdout och signalerar resultatet via sin
exit-kod, så flödet kan grena på "allt attesterat" / "det finns
tidrapporter som väntar".

## Vad den gör

1. Hämtar en åtkomsttoken via `POST /v1/Auth/Token` (Basic-auth med
   applikations-id/secret).
2. Hämtar alla tidrapporter för innevarande (eller angiven) månad via
   `GET /v1/Core/TimeReports`, paginerat, med automatisk backoff/retry vid
   `429 Too Many Requests` (API:et tillåter max 4 anrop/sekund).
3. Härleder attestläget per tidrapport från fälten `sentToAttestDate` /
   `attestedDate`:
   - **notSentToAttest** – inte ens skickad till attest.
   - **sentAwaitingAttest** – skickad, väntar på attestant.
   - **attested** – attesterad.
4. Summerar totalt och per användare, skriver ut resultatet och avslutar med
   en exit-kod som beskriver läget.

## Bygga

Kräver en C-kompilator och libcurls dev-headers.

```sh
sudo apt-get install build-essential libcurl4-openssl-dev   # Debian/Ubuntu
make
```

Binären hamnar som `./blikk-attest-check`. `third_party/cJSON.{c,h}` är
[cJSON](https://github.com/DaveGamble/cJSON) (MIT-licens), vendorat i
repot så att bygget inte behöver hämta något vid kompilering.

## Konfiguration

Programmet behöver en Blikk API-applikation (id + secret). Skapas av en
administratör i Blikk under integrations-/API-inställningarna; se till att
applikationen har behörigheten `Timereport_Read`.

| Miljövariabel      | Motsvarande flagga | Beskrivning                                              |
|--------------------|---------------------|-----------------------------------------------------------|
| `BLIKK_APP_ID`      | `--app-id`          | Applikations-id (obligatorisk)                            |
| `BLIKK_APP_SECRET`  | `--app-secret`      | Applikationens secret (obligatorisk)                       |
| `BLIKK_USER_IDS`    | `--user-id` (kan upprepas) | Kommaseparerad lista med Blikk-användar-id:n att kontrollera. Utelämnas filtret returneras alla tidrapporter appen har läsbehörighet till. |
| `BLIKK_BASE_URL`    | `--base-url`        | Override av bas-URL. Standard: `https://publicapi.blikk.com` |

Användar-id:n hittas t.ex. via `GET /v1/Core/Users` eller i Blikks
admin-gränssnitt. Håll `BLIKK_APP_SECRET` som en hemlighet i flödets/
plattformens secret store — lägg den aldrig i klartext i ett skript eller
en commit.

## Användning

```sh
export BLIKK_APP_ID=...
export BLIKK_APP_SECRET=...
export BLIKK_USER_IDS=1,42          # valfritt: begränsa till specifika användare

./blikk-attest-check                # innevarande månad, JSON på stdout
./blikk-attest-check --format text  # läsbart format
./blikk-attest-check --month 2026-08 --user-id 1
```

Fullständig flagglista: `./blikk-attest-check --help`.

### Exit-koder

| Kod | Betydelse                                                           |
|-----|----------------------------------------------------------------------|
| 0   | Anropet lyckades och samtliga tidrapporter i perioden är attesterade |
| 1   | Anropet lyckades, men minst en tidrapport är inte (ännu) attesterad, eller inga tidrapporter alls hittades för perioden |
| 2   | Fel användning: ogiltiga flaggor eller saknad konfiguration           |
| 3   | Fel mot Blikk-API:et: inloggning eller förfrågan misslyckades         |

Ett automationsflöde kan i regel bara bry sig om `0` (allt klart) kontra
`≠0` (kolla vidare), men skillnaden mellan 1/2/3 gör det enkelt att larma
olika beroende på om det är en riktig attestrestpost eller ett trasigt
autentiseringssteg.

### JSON-utdata

```json
{
  "objectName": "blikkAttestCheck.summary",
  "generatedAt": "2026-09-09T10:00:00Z",
  "month": "2026-09",
  "from": "2026-09-01",
  "to": "2026-09-30",
  "totalReports": 20,
  "totalHours": 160.5,
  "notSentToAttest": 2,
  "sentAwaitingAttest": 3,
  "attested": 15,
  "allAttested": false,
  "users": [
    {
      "userId": 1,
      "userName": "John Doe",
      "totalReports": 20,
      "totalHours": 160.5,
      "notSentToAttest": 2,
      "sentAwaitingAttest": 3,
      "attested": 15,
      "allAttested": false
    }
  ],
  "pendingReports": [
    { "id": 4711, "date": "2026-09-05", "userId": 1, "userName": "John Doe",
      "hours": 8, "status": "notSentToAttest" }
  ]
}
```

`pendingReports` innehåller varje ej attesterad tidrapport, så flödet kan
t.ex. bygga ett Teams-/mejlmeddelande med exakt vilka dagar som saknas.

## Använda i automationsflöden

Binären är tänkt att köras av en agent som redan finns i flödet:

- **Power Automate (Desktop flow / Copilot Studio)**: kör binären med
  åtgärden "Kör DOS-kommando" / "Run application" på en maskin med
  binären installerad, fånga stdout i en variabel, tolka den med
  "Analysera JSON" och grena på `%ExitCode%` respektive `allAttested`.
- **Google (Apps Script / Cloud Scheduler)**: paketera binären i en
  container och kör den som ett schemalagt Cloud Run-jobb, eller kör den
  på en Compute Engine-instans/valfri VM som Apps Script kan trigga via
  HTTP; låt jobbet skicka `pendingReports` vidare till t.ex. Gmail/Chat.
- **Vanlig cron/CI**: kör binären som ett steg, kontrollera exit-koden
  direkt i skalet.

## Källa till API-detaljer

Endpoints, filter och fältnamn i den här implementationen är hämtade från
den publika Blikk API-dokumentationen:
<https://publicapidocs.blikk.com/#core-resources-timereports-list>.
`filter.userIds` skickas som upprepad query-parameter
(`filter.userIds=1&filter.userIds=2`), vilket är den vanliga ASP.NET
Web API-konventionen för array-filter — dokumentationen visar inte ett
konkret exempel på detta, så justera `append_user_id_filters()` i
`src/blikk_api.c` om ditt Blikk-konto förväntar sig ett annat format.

## Licens

MIT, se [LICENSE](LICENSE). Innehåller [cJSON](https://github.com/DaveGamble/cJSON)
(MIT), se [third_party/LICENSE_cJSON.txt](third_party/LICENSE_cJSON.txt).
