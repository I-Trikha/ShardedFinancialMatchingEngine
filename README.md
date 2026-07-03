# ⚡ NexusCore

> A high-performance stock exchange matching engine simulator with a real-time trading dashboard.

NexusCore is a browser-based simulation of a modern electronic stock exchange. It features a live order book, order matching engine, market depth visualization, trade execution feed, and exchange telemetry, providing an interactive demonstration of how financial exchanges process buy and sell orders.

The project is designed to showcase concepts behind low-latency trading systems while maintaining an intuitive and responsive web interface.

---

## Features

### 📈 Order Matching Engine

- Limit Orders
- Market Orders
- Order Cancellation
- Price-Time Priority Matching
- Partial Order Fills

### 📊 Live Trading Dashboard

- Real-time Order Book
- Market Depth Chart
- Trade Feed
- Bid-Ask Spread
- Live Ticker Prices

### 🚀 Exchange Telemetry

- Orders Processed
- Trades Executed
- Average Latency
- Throughput
- Peak Latency
- Pool Utilization

### 🎨 Interactive UI

- Responsive dashboard
- Live order book updates
- Animated market depth chart
- Multi-stock support
- Buy/Sell order entry
- Trading simulation controls

---

## Screenshots

| Dashboard | Order Book | Market Depth |
|------------|------------|--------------|
| *(Add Screenshot)* | *(Add Screenshot)* | *(Add Screenshot)* |

---

## Architecture

```

                     User Orders
                          │
                          ▼
                  Order Entry Panel
                          │
                          ▼
                  Matching Engine
        ┌─────────────────┴─────────────────┐
        ▼                                   ▼
 Order Book Update                 Trade Execution
        │                                   │
        └──────────────┬────────────────────┘
                       ▼
              Dashboard Rendering
                       │
                       ▼
      Order Book • Depth Chart • Trade Feed

```

---

## Supported Order Types

### Limit Order

Executes only at the specified price or better.

### Market Order

Executes immediately against the best available price.

### Cancel Order

Cancels an existing order from the order book.

---

## Matching Algorithm

The engine follows **Price-Time Priority**, the standard mechanism used by modern stock exchanges.

Matching rules:

1. Best price has highest priority
2. Earlier orders have higher priority
3. Partial fills supported
4. Remaining quantity is added back to the order book (for limit orders)

---

## Market Simulation

The simulator continuously generates realistic market activity including:

- Random buy/sell orders
- Price fluctuations
- Liquidity changes
- Trade executions
- Order cancellations

This creates a dynamic environment for visualizing exchange behavior.

---

## Dashboard Components

### 📚 Order Book

Displays the top bid and ask levels with cumulative quantities.

### 📉 Market Depth

Interactive depth chart visualizing cumulative buy and sell liquidity.

### 📃 Trade Feed

Shows recently executed trades including:

- Symbol
- Side
- Price
- Quantity

### 📡 Telemetry

Real-time exchange statistics including:

- Orders processed
- Trades executed
- Throughput
- Average latency
- Peak latency

---

## Supported Stocks

- AAPL
- TSLA
- MSFT

The architecture allows additional instruments to be added easily.

---

## Technologies Used

- HTML5
- CSS3
- Vanilla JavaScript
- Canvas API
- DOM Manipulation

No external frontend framework is required.

---

## Project Structure

```

NexusCore/
│
├── index.html
├── README.md
├── LICENSE
├── .gitignore
│
├── screenshots/
│   ├── dashboard.png
│   ├── orderbook.png
│   └── depth-chart.png
│
├── assets/
│   └── logo.png
│
└── docs/
    └── architecture.png

```

---

## Running the Project

Clone the repository

```bash
git clone https://github.com/<username>/NexusCore.git
```

Open

```
index.html
```

in your browser.

No installation or dependencies are required.

---

## Future Improvements

- Price-level queues
- Time priority using linked lists
- WebSocket server
- Multi-user trading
- Persistent order storage
- Historical candlestick charts
- Portfolio management
- Stop-loss and stop-limit orders
- Market statistics API
- Backend matching engine in C++

---

## Learning Outcomes

This project demonstrates understanding of:

- Exchange matching algorithms
- Data structures
- Event-driven programming
- Real-time UI updates
- Interactive data visualization
- Low-latency system design

---

## Author

**Ishan Trikha**

B.Tech Mechanical Engineering

Indian Institute of Technology Kanpur

---

## License

MIT License
