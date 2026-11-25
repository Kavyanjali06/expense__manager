"use strict";

const currency = new Intl.NumberFormat("en-IN", {
  style: "currency",
  currency: "INR",
  minimumFractionDigits: 2,
});
const shortCurrency = new Intl.NumberFormat("en-IN", {
  style: "currency",
  currency: "INR",
  maximumFractionDigits: 0,
});
const categoryColors = [
  "#69dda5",
  "#a89bff",
  "#68b9ec",
  "#f2bd6d",
  "#fa8292",
  "#50c9bb",
  "#d184e9",
  "#a1b66f",
];
const STORAGE_KEY = "fintrack.transactions.v1";

const elements = {
  today: document.querySelector("#today-label"),
  balance: document.querySelector("#balance-value"),
  income: document.querySelector("#income-value"),
  expenses: document.querySelector("#expenses-value"),
  monthExpenses: document.querySelector("#month-expenses-value"),
  monthCaption: document.querySelector("#month-caption"),
  monthNet: document.querySelector("#month-net"),
  monthNetCaption: document.querySelector("#month-net-caption"),
  cashflowChart: document.querySelector("#cashflow-chart"),
  categoryChart: document.querySelector("#category-chart"),
  categoryTotal: document.querySelector("#category-total"),
  categoryLegend: document.querySelector("#category-legend"),
  transactionList: document.querySelector("#transaction-list"),
  emptyState: document.querySelector("#empty-state"),
  tableFooter: document.querySelector("#table-footer"),
  filter: document.querySelector("#transaction-filter"),
  importFile: document.querySelector("#import-file"),
  importButton: document.querySelector("#import-transactions"),
  exportButton: document.querySelector("#export-transactions"),
  dialog: document.querySelector("#transaction-dialog"),
  form: document.querySelector("#transaction-form"),
  formError: document.querySelector("#form-error"),
  saveButton: document.querySelector("#save-transaction"),
  toast: document.querySelector("#toast"),
};

let transactions = [];
let summary = null;
let backendMode = false;
let toastTimer;

function localDateString(date = new Date()) {
  const year = date.getFullYear();
  const month = String(date.getMonth() + 1).padStart(2, "0");
  const day = String(date.getDate()).padStart(2, "0");
  return `${year}-${month}-${day}`;
}

function formatDate(value) {
  const [year, month, day] = value.split("-").map(Number);
  return new Intl.DateTimeFormat("en-US", {
    month: "short",
    day: "numeric",
    year: "numeric",
  }).format(new Date(year, month - 1, day));
}

function setText(element, value) {
  element.textContent = value;
}

async function request(path, options = {}) {
  const response = await fetch(path, {
    ...options,
    headers: {
      ...(options.body ? { "Content-Type": "application/json" } : {}),
      ...options.headers,
    },
  });
  if (!response.headers.get("content-type")?.includes("application/json")) {
    throw new Error("A JSON API is not available on this host.");
  }
  const result = await response.json();
  if (!response.ok) {
    throw new Error(result.error || "The request could not be completed.");
  }
  return result;
}

function createSummary(items) {
  const currentMonth = localDateString().slice(0, 7);
  const monthly = [];
  for (let offset = 5; offset >= 0; offset -= 1) {
    const date = new Date();
    date.setDate(1);
    date.setHours(12, 0, 0, 0);
    date.setMonth(date.getMonth() - offset);
    monthly.push({
      key: `${date.getFullYear()}-${String(date.getMonth() + 1).padStart(2, "0")}`,
      month: new Intl.DateTimeFormat("en-US", { month: "short" }).format(date),
      income: 0,
      expenses: 0,
    });
  }

  const categories = new Map();
  const result = {
    balance: 0,
    income: 0,
    expenses: 0,
    monthIncome: 0,
    monthExpenses: 0,
    month: currentMonth,
    transactionCount: items.length,
    monthly,
    categories: [],
  };
  for (const transaction of items) {
    const isIncome = transaction.type === "Income";
    const monthKey = transaction.date.slice(0, 7);
    if (isIncome) {
      result.income += transaction.amount;
      result.balance += transaction.amount;
    } else {
      result.expenses += transaction.amount;
      result.balance -= transaction.amount;
      categories.set(
        transaction.category,
        (categories.get(transaction.category) || 0) + transaction.amount,
      );
    }

    const month = monthly.find((item) => item.key === monthKey);
    if (month) {
      month[isIncome ? "income" : "expenses"] += transaction.amount;
    }
    if (monthKey === currentMonth) {
      result[isIncome ? "monthIncome" : "monthExpenses"] += transaction.amount;
    }
  }
  result.categories = [...categories.entries()]
    .map(([name, amount]) => ({ name, amount }))
    .sort((left, right) => right.amount - left.amount);
  return result;
}

function isValidDate(value) {
  if (!/^\d{4}-\d{2}-\d{2}$/.test(value)) return false;
  const [year, month, day] = value.split("-").map(Number);
  const date = new Date(year, month - 1, day, 12);
  return date.getFullYear() === year &&
    date.getMonth() === month - 1 &&
    date.getDate() === day;
}

function validateTransaction(transaction) {
  if (!transaction || typeof transaction !== "object") {
    throw new Error("A transaction row is invalid.");
  }
  const id = Number(transaction.id);
  const amount = Number(transaction.amount);
  const date = String(transaction.date ?? "");
  const type = String(transaction.type ?? "");
  const category = String(transaction.category ?? "").trim();
  const description = String(transaction.description ?? "").trim();
  if (!Number.isSafeInteger(id) || id < 1 ||
      !isValidDate(date) || (type !== "Income" && type !== "Expense") ||
      !category || !description || category.includes("|") ||
      description.includes("|") || !Number.isFinite(amount) || amount <= 0) {
    throw new Error("Transaction details are invalid. Check the date, type, text, and positive amount.");
  }
  return { id, date, type, category, description, amount };
}

function loadTransactions() {
  const saved = localStorage.getItem(STORAGE_KEY);
  if (saved === null) return [];
  const parsed = JSON.parse(saved);
  if (!Array.isArray(parsed)) {
    throw new Error("Saved FinTrack data is invalid. Import a valid transactions text file.");
  }
  const loaded = parsed.map(validateTransaction);
  const ids = new Set(loaded.map((transaction) => transaction.id));
  if (ids.size !== loaded.length) {
    throw new Error("Saved FinTrack data contains duplicate transaction IDs.");
  }
  return loaded;
}

function persistTransactions() {
  localStorage.setItem(STORAGE_KEY, JSON.stringify(transactions));
}

function parseTextFile(contents) {
  const rows = contents.split(/\r?\n/).filter((line) => line.trim() !== "");
  const imported = rows.map((line) => {
    const fields = line.split("|");
    if (fields.length !== 6) {
      throw new Error("A text-file row does not contain six | separated fields.");
    }
    return validateTransaction({
      id: fields[0],
      date: fields[1],
      type: fields[2],
      category: fields[3],
      description: fields[4],
      amount: fields[5],
    });
  });
  const ids = new Set(imported.map((transaction) => transaction.id));
  if (ids.size !== imported.length) {
    throw new Error("The text file contains duplicate transaction IDs.");
  }
  return imported;
}

function exportTextFile() {
  const rows = [...transactions]
    .sort((left, right) => left.id - right.id)
    .map((transaction) => [
      transaction.id,
      transaction.date,
      transaction.type,
      transaction.category,
      transaction.description,
      transaction.amount.toFixed(2),
    ].join("|"));
  const file = new Blob([rows.length ? `${rows.join("\n")}\n` : ""], {
    type: "text/plain;charset=utf-8",
  });
  const url = URL.createObjectURL(file);
  const link = document.createElement("a");
  link.href = url;
  link.download = "transactions.txt";
  link.click();
  URL.revokeObjectURL(url);
}

async function importTextFile(file) {
  if (backendMode) {
    throw new Error("Text import is available on the Vercel browser-storage edition.");
  }
  if (file.size > 2 * 1024 * 1024) {
    throw new Error("The text file is larger than 2 MB.");
  }
  const imported = parseTextFile(await file.text());
  if (!window.confirm(`Replace your current ${transactions.length} transaction(s) with ${imported.length} imported transaction(s)?`)) {
    return;
  }
  transactions = imported;
  persistTransactions();
  await refreshDashboard();
  showToast(`${imported.length} transaction(s) imported.`);
}

function showToast(message, isError = false) {
  clearTimeout(toastTimer);
  elements.toast.textContent = message;
  elements.toast.classList.toggle("error-toast", isError);
  elements.toast.classList.add("visible");
  toastTimer = setTimeout(() => elements.toast.classList.remove("visible"), 3200);
}

function updateSummary() {
  setText(elements.balance, currency.format(summary.balance));
  setText(elements.income, currency.format(summary.income));
  setText(elements.expenses, currency.format(summary.expenses));
  setText(elements.monthExpenses, currency.format(summary.monthExpenses));
  setText(
    elements.monthCaption,
    new Intl.DateTimeFormat("en-US", { month: "long", year: "numeric" })
      .format(new Date(`${summary.month}-01T12:00:00`)),
  );
  const net = summary.monthIncome - summary.monthExpenses;
  setText(elements.monthNet, currency.format(net));
  elements.monthNet.style.color = net >= 0 ? "var(--green)" : "var(--red)";
  setText(elements.monthNetCaption, "Net this month");
}

function renderCashflow() {
  const highest = Math.max(
    1,
    ...summary.monthly.flatMap((month) => [month.income, month.expenses]),
  );
  elements.cashflowChart.replaceChildren();

  for (const month of summary.monthly) {
    const column = document.createElement("div");
    column.className = "bar-month";
    if (month.key === summary.month) {
      column.classList.add("current");
    }

    const pair = document.createElement("div");
    pair.className = "bar-pair";
    for (const [type, value] of [["income", month.income], ["expense", month.expenses]]) {
      const bar = document.createElement("span");
      bar.className = `bar ${type}`;
      bar.style.height = value > 0 ? `${Math.max(3, (value / highest) * 100)}%` : "2px";
      bar.title = `${type === "income" ? "Income" : "Expenses"}: ${currency.format(value)}`;
      pair.append(bar);
    }

    const label = document.createElement("span");
    label.className = "bar-month-label";
    label.textContent = month.month;
    column.append(pair, label);
    elements.cashflowChart.append(column);
  }
}

function renderCategories() {
  const categories = summary.categories;
  const total = categories.reduce((sum, item) => sum + item.amount, 0);
  setText(elements.categoryTotal, shortCurrency.format(total));
  elements.categoryLegend.replaceChildren();

  if (!categories.length || total <= 0) {
    elements.categoryChart.style.background = "conic-gradient(#2c313c 0deg 360deg)";
    const empty = document.createElement("span");
    empty.className = "category-empty";
    empty.textContent = "Your spending categories will show up here.";
    elements.categoryLegend.append(empty);
    return;
  }

  let degrees = 0;
  const slices = categories.map((item, index) => {
    const start = degrees;
    degrees += (item.amount / total) * 360;
    return `${categoryColors[index % categoryColors.length]} ${start}deg ${degrees}deg`;
  });
  elements.categoryChart.style.background = `conic-gradient(${slices.join(", ")})`;

  for (const [index, item] of categories.slice(0, 5).entries()) {
    const row = document.createElement("div");
    row.className = "category-item";
    const swatch = document.createElement("span");
    swatch.className = "category-swatch";
    swatch.style.background = categoryColors[index % categoryColors.length];
    const name = document.createElement("span");
    name.className = "category-name";
    name.textContent = item.name;
    name.title = item.name;
    const amount = document.createElement("span");
    amount.className = "category-amount";
    amount.textContent = shortCurrency.format(item.amount);
    row.append(swatch, name, amount);
    elements.categoryLegend.append(row);
  }
  if (categories.length > 5) {
    const more = document.createElement("span");
    more.className = "category-empty";
    more.textContent = `+ ${categories.length - 5} more categories`;
    elements.categoryLegend.append(more);
  }
}

function transactionRow(transaction) {
  const row = document.createElement("tr");
  const nameCell = document.createElement("td");
  const nameWrap = document.createElement("div");
  nameWrap.className = "transaction-name";
  const symbol = document.createElement("span");
  symbol.className = `transaction-symbol${transaction.type === "Income" ? " income-symbol" : ""}`;
  symbol.setAttribute("aria-hidden", "true");
  symbol.textContent = transaction.type === "Income" ? "↙" : "↗";
  const copy = document.createElement("span");
  copy.className = "transaction-copy";
  const description = document.createElement("strong");
  description.textContent = transaction.description;
  description.title = transaction.description;
  const type = document.createElement("span");
  type.textContent = transaction.type;
  copy.append(description, type);
  nameWrap.append(symbol, copy);
  nameCell.append(nameWrap);

  const categoryCell = document.createElement("td");
  const category = document.createElement("span");
  category.className = "category-pill";
  category.textContent = transaction.category;
  category.title = transaction.category;
  categoryCell.append(category);

  const dateCell = document.createElement("td");
  dateCell.className = "date-cell";
  dateCell.textContent = formatDate(transaction.date);

  const amountCell = document.createElement("td");
  amountCell.className = `amount-cell ${transaction.type === "Income" ? "income-amount" : "expense-amount"}`;
  amountCell.textContent = `${transaction.type === "Income" ? "+" : "−"}${currency.format(transaction.amount)}`;

  const actionCell = document.createElement("td");
  const deleteButton = document.createElement("button");
  deleteButton.className = "delete-button";
  deleteButton.type = "button";
  deleteButton.textContent = "×";
  deleteButton.setAttribute("aria-label", `Delete ${transaction.description}`);
  deleteButton.title = "Delete transaction";
  deleteButton.addEventListener("click", () => removeTransaction(transaction));
  actionCell.append(deleteButton);
  row.append(nameCell, categoryCell, dateCell, amountCell, actionCell);
  return row;
}

function renderTransactions() {
  const selectedType = elements.filter.value;
  const visible = transactions
    .filter((transaction) => selectedType === "all" || transaction.type === selectedType)
    .sort((left, right) => right.date.localeCompare(left.date) || right.id - left.id);
  elements.transactionList.replaceChildren(...visible.slice(0, 8).map(transactionRow));
  elements.emptyState.hidden = visible.length > 0;
  setText(
    elements.tableFooter,
    visible.length
      ? `Showing ${Math.min(visible.length, 8)} of ${visible.length} ${visible.length === 1 ? "transaction" : "transactions"}`
      : "",
  );
}

async function refreshDashboard() {
  if (backendMode) {
    const [transactionData, summaryData] = await Promise.all([
      request("/api/transactions"),
      request("/api/summary"),
    ]);
    transactions = transactionData.transactions;
    summary = summaryData;
  } else {
    summary = createSummary(transactions);
  }
  updateSummary();
  renderCashflow();
  renderCategories();
  renderTransactions();
}

function openDialog() {
  elements.form.reset();
  document.querySelector("#date").value = localDateString();
  document.querySelector("#transaction-type").value = "Expense";
  document.querySelectorAll(".type-button").forEach((button) => {
    button.classList.toggle("selected", button.dataset.type === "Expense");
  });
  elements.formError.hidden = true;
  elements.dialog.showModal();
  document.querySelector("#amount").focus();
}

async function removeTransaction(transaction) {
  const okay = window.confirm(
    `Delete ${transaction.type.toLowerCase()} "${transaction.description}" for ${currency.format(transaction.amount)}?`,
  );
  if (!okay) return;
  try {
    if (backendMode) {
      await request(`/api/transactions/${transaction.id}`, { method: "DELETE" });
    } else {
      transactions = transactions.filter((item) => item.id !== transaction.id);
      persistTransactions();
    }
    await refreshDashboard();
    showToast("Transaction deleted.");
  } catch (error) {
    showToast(error.message, true);
  }
}

async function saveTransaction(event) {
  event.preventDefault();
  elements.formError.hidden = true;
  elements.saveButton.disabled = true;
  elements.saveButton.textContent = "Saving…";

  const formData = new FormData(elements.form);
  const transaction = {
    type: formData.get("type"),
    amount: Number(formData.get("amount")),
    category: String(formData.get("category")).trim(),
    date: formData.get("date"),
    description: String(formData.get("description")).trim(),
  };

  try {
    if (backendMode) {
      await request("/api/transactions", {
        method: "POST",
        body: JSON.stringify(transaction),
      });
    } else {
      const largestId = transactions.reduce((largest, item) => Math.max(largest, item.id), 0);
      transactions.push(validateTransaction({ ...transaction, id: largestId + 1 }));
      persistTransactions();
    }
    elements.dialog.close();
    await refreshDashboard();
    showToast(`${transaction.type} added successfully.`);
  } catch (error) {
    elements.formError.textContent = error.message;
    elements.formError.hidden = false;
  } finally {
    elements.saveButton.disabled = false;
    elements.saveButton.textContent = "Save transaction";
  }
}

function initialize() {
  setText(
    elements.today,
    new Intl.DateTimeFormat("en-US", {
      weekday: "short",
      month: "short",
      day: "numeric",
      year: "numeric",
    }).format(new Date()),
  );
  document.querySelector("#date").value = localDateString();
  document.querySelector("#open-dialog").addEventListener("click", openDialog);
  document.querySelector("#open-dialog-secondary").addEventListener("click", openDialog);
  document.querySelector("#empty-add").addEventListener("click", openDialog);
  document.querySelector("#close-dialog").addEventListener("click", () => elements.dialog.close());
  document.querySelector("#cancel-dialog").addEventListener("click", () => elements.dialog.close());
  document.querySelectorAll(".type-button").forEach((button) => {
    button.addEventListener("click", () => {
      document.querySelectorAll(".type-button").forEach((item) => item.classList.remove("selected"));
      button.classList.add("selected");
      document.querySelector("#transaction-type").value = button.dataset.type;
    });
  });
  elements.form.addEventListener("submit", saveTransaction);
  elements.filter.addEventListener("change", renderTransactions);
  elements.exportButton.addEventListener("click", exportTextFile);
  elements.importButton.addEventListener("click", () => elements.importFile.click());
  elements.importFile.addEventListener("change", async () => {
    const [file] = elements.importFile.files;
    elements.importFile.value = "";
    if (!file) return;
    try {
      await importTextFile(file);
    } catch (error) {
      showToast(error.message, true);
    }
  });
  document.querySelectorAll(".nav-link").forEach((link) => {
    link.addEventListener("click", () => {
      document.querySelectorAll(".nav-link").forEach((item) => {
        item.classList.remove("active");
        item.removeAttribute("aria-current");
      });
      link.classList.add("active");
      link.setAttribute("aria-current", "page");
    });
  });
  (async () => {
    try {
      await request("/api/transactions");
      backendMode = true;
      elements.importButton.disabled = true;
      elements.importButton.title = "Text import is for the Vercel browser-storage edition.";
    } catch {
      try {
        transactions = loadTransactions();
      } catch (storageError) {
        showToast(storageError.message, true);
      }
    }
    try {
      await refreshDashboard();
    } catch (error) {
      showToast(error.message, true);
    }
  })();
}

initialize();
