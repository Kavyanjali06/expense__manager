# FinTrack

FinTrack is a personal finance dashboard with a C++17 local backend and an
HTML/CSS/JavaScript frontend. The same frontend supports two storage modes:

- **Run locally:** C++ serves the app and saves transactions to
  `data/transactions.txt` using `ifstream` and `ofstream`.
- **Deploy to Vercel:** Vercel serves the static frontend; transaction data is
  stored in that browser's local storage. Use **Import .txt** and **Export
  .txt** to move or back up data. Browser storage does not sync across devices.

The local text-file contents are excluded from Git and must not be uploaded as
part of the source project.

## Features

- Dark responsive dashboard with Indian rupee (INR) formatting
- Add and delete income and expense transactions
- All-time totals, current-month summary, six-month cash-flow chart, and
  spending-by-category chart
- Filter recent transactions by income or expense
- Text import/export on the static hosted edition
- No third-party JavaScript, packages, or database

## Project files

```text
FinTrack/
|-- main.cpp
|-- README.md
|-- .gitignore
|-- vercel.json
|-- web/
|   |-- index.html
|   |-- styles.css
|   `-- app.js
`-- data/
    `-- transactions.txt  (local only; ignored by Git)
```

## Run locally on Windows

Requirements: MinGW-w64 g++ and a modern browser.

1. Open this folder in VS Code and select **Terminal > New Terminal**.
2. Compile the C++ backend:

   ```powershell
   g++ -std=c++17 -Wall -Wextra -pedantic main.cpp -o fintrack.exe -lws2_32
   ```

3. Run the server:

   ```powershell
   .\fintrack.exe
   ```

4. Open **http://127.0.0.1:8080**.
5. Add transactions from the dashboard. The C++ backend saves them in
   `data/transactions.txt`.
6. Leave the terminal open while using the app. Press **Ctrl+C** in that
   terminal to stop it.

If port 8080 is already in use, pass another port, for example
`.\fintrack.exe 8081`, then visit **http://127.0.0.1:8081**.

## Deploy the static edition to Vercel

The Vercel edition is intentionally static. Vercel does not run this
long-lived C++ server or persist its local text file. The static edition uses
browser local storage and offers text-file import/export instead.

1. Create a **private** GitHub repository named `FinTrack`.
2. From the project folder, initialize Git, add the source files, and push the
   `main` branch:

   ```powershell
   git init
   git add .gitignore README.md main.cpp vercel.json web
   git commit -m "Add FinTrack personal finance dashboard"
   git branch -M main
   git remote add origin https://github.com/YOUR-USERNAME/FinTrack.git
   git push -u origin main
   ```

   Do not add `data/transactions.txt`, `fintrack.exe`, or any other personal
   finance data to Git.

3. Sign in to Vercel, choose **Add New → Project**, and import the `FinTrack`
   GitHub repository.
4. Use the repository root as the project root. `vercel.json` sets `web` as
   the static output directory, so no build command or environment variables
   are needed.
5. Select **Deploy**. Vercel will provide the public deployment URL.
6. On the deployed site, use **Export .txt** to back up browser data and
   **Import .txt** to restore it. Import replaces the current browser data only
   after a confirmation prompt.

The repository is private by default, but a Vercel URL is normally shareable.
Browser-stored finances stay in the individual browser profile; anyone with
access to that profile can use them. Do not enter sensitive information on a
shared device.

## Text-file format

The local C++ version stores one transaction per line:

```text
1|2026-11-25|Expense|Food|Lunch|12.50
2|2026-11-25|Income|Salary|Monthly pay|2500.00
```

Fields are `id|date|type|category|description|amount`. Use `YYYY-MM-DD` dates,
decimal points for amounts, and avoid `|` in category and description text.
The hosted edition imports and exports this same format.

## C++ OOP concepts

- **Encapsulation:** private transaction fields with read-only accessors.
- **Abstraction:** abstract `Transaction` and `TransactionStore` interfaces.
- **Inheritance:** `IncomeTransaction` and `ExpenseTransaction` extend
  `Transaction`.
- **Polymorphism:** income and expense subclasses override transaction type
  and signed-amount behavior.
