import { useCallback, useEffect, useMemo, useState, type ReactNode } from 'react'
import './App.css'

type Device = {
  client_id: string
  connected: boolean
  latest_point: number | null
  timestamp_unix_ms: number
}

type Event = {
  client_id: string
  point: number
  timestamp_unix_ms: number
}

type DashboardData = {
  threshold: number
  devices: Device[]
  events: Event[]
}

type Layout = {
  left: string[]
  right: string[]
}

const initialLayout: Layout = {
  left: ['configuration', 'threshold', 'calibration', 'devices'],
  right: ['chart', 'history'],
}

const colors = ['#45464b', '#2196ed', '#dc554d', '#34a36a', '#945bb5', '#f39c36']

function readLayout(): Layout {
  try {
    const saved = JSON.parse(localStorage.getItem('measure-dashboard-layout') || 'null') as Layout | null
    if (saved && Array.isArray(saved.left) && Array.isArray(saved.right)) {
      const valid = new Set([...initialLayout.left, ...initialLayout.right])
      const left = saved.left.filter(key => valid.has(key))
      const right = saved.right.filter(key => valid.has(key) && !left.includes(key))
      for (const key of valid) {
        if (!left.includes(key) && !right.includes(key)) right.push(key)
      }
      return { left, right }
    }
  } catch {
    // Ignore invalid saved layouts and use the defaults.
  }
  return initialLayout
}

function timeLabel(timestamp: number) {
  return timestamp ? new Date(timestamp).toLocaleString() : '—'
}

function Panel({ id, title, children, onDragStart, onDrop }: {
  id: string
  title: string
  children: ReactNode
  onDragStart: (id: string) => void
  onDrop: (target: string) => void
}) {
  return (
    <section
      id={`panel-${id}`}
      className="panel"
      draggable
      onDragStart={() => onDragStart(id)}
      onDragOver={event => event.preventDefault()}
      onDrop={() => onDrop(id)}
    >
      <div className="panel-title">
        <span>{title}</span>
        <span className="panel-tools" aria-hidden="true">●　↻　⌃　⛶</span>
      </div>
      {children}
    </section>
  )
}

function Chart({ events }: { events: Event[] }) {
  const sorted = useMemo(
    () => events.slice().sort((a, b) => a.timestamp_unix_ms - b.timestamp_unix_ms).slice(-120),
    [events],
  )
  if (!sorted.length) {
    return <div className="chart-empty">No measurements have been recorded yet.</div>
  }

  const width = 1200
  const height = 290
  const margins = { left: 62, right: 30, top: 16, bottom: 42 }
  const values = sorted.map(item => item.point)
  const min = Math.min(...values)
  const max = Math.max(...values)
  const valueSpan = Math.max(max - min, 1)
  const start = sorted[0].timestamp_unix_ms
  const timeSpan = Math.max(sorted[sorted.length - 1].timestamp_unix_ms - start, 1)
  const x = (event: Event) => margins.left +
    ((event.timestamp_unix_ms - start) / timeSpan) * (width - margins.left - margins.right)
  const y = (event: Event) => height - margins.bottom -
    ((event.point - min) / valueSpan) * (height - margins.top - margins.bottom)
  const clients = [...new Set(sorted.map(item => item.client_id))]

  return (
    <div className="chart-wrap">
      <svg className="chart" viewBox={`0 0 ${width} ${height}`} role="img" aria-label="Recent measurements by device">
        {[0, 0.5, 1].map(fraction => {
          const lineY = margins.top + fraction * (height - margins.top - margins.bottom)
          const label = Math.round(max - fraction * valueSpan)
          return (
            <g key={fraction}>
              <line x1={margins.left} x2={width - margins.right} y1={lineY} y2={lineY} className="grid-line" />
              <text x={margins.left - 12} y={lineY + 5} textAnchor="end" className="axis-label">{label}</text>
            </g>
          )
        })}
        <line x1={margins.left} x2={width - margins.right} y1={height - margins.bottom} y2={height - margins.bottom} className="axis-line" />
        {clients.map((client, index) => {
          const points = sorted.filter(item => item.client_id === client)
          const path = points.map((event, pointIndex) =>
            `${pointIndex === 0 ? 'M' : 'L'} ${x(event).toFixed(1)} ${y(event).toFixed(1)}`,
          ).join(' ')
          return <path key={client} d={path} fill="none" stroke={colors[index % colors.length]} strokeWidth="2.5" />
        })}
        <text x={margins.left} y={height - 12} className="axis-label">{new Date(start).toLocaleTimeString()}</text>
        <text x={width - margins.right} y={height - 12} textAnchor="end" className="axis-label">
          {new Date(sorted[sorted.length - 1].timestamp_unix_ms).toLocaleTimeString()}
        </text>
      </svg>
      <div className="legend">
        {clients.map((client, index) => (
          <span key={client}><i style={{ backgroundColor: colors[index % colors.length] }} />{client}</span>
        ))}
      </div>
    </div>
  )
}

function App() {
  const [data, setData] = useState<DashboardData>({ threshold: 8, devices: [], events: [] })
  const [layout, setLayout] = useState<Layout>(readLayout)
  const [notice, setNotice] = useState('')
  const [threshold, setThreshold] = useState('8')
  const [target, setTarget] = useState('')
  const [duration, setDuration] = useState('10')
  const [dragged, setDragged] = useState<string | null>(null)
  const [now, setNow] = useState<number | null>(null)

  const refresh = useCallback(async () => {
    try {
      const response = await fetch('/api/dashboard', { cache: 'no-store' })
      if (!response.ok) throw new Error(`HTTP ${response.status}`)
      const current = await response.json() as DashboardData
      setData(current)
      setThreshold(value => value === String(data.threshold) ? String(current.threshold) : value)
    } catch (error) {
      setNotice(`Dashboard refresh failed: ${String(error)}`)
    }
  }, [data.threshold])

  useEffect(() => {
    const initial = window.setTimeout(() => void refresh(), 0)
    const poll = window.setInterval(() => void refresh(), 2000)
    const clock = window.setInterval(() => setNow(Date.now()), 1000)
    return () => {
      window.clearTimeout(initial)
      window.clearInterval(poll)
      window.clearInterval(clock)
    }
  }, [refresh])

  const movePanel = (targetId: string) => {
    if (!dragged || dragged === targetId) return
    setLayout(current => {
      const sourceColumn = current.left.includes(dragged) ? 'left' : 'right'
      const targetColumn = current.left.includes(targetId) ? 'left' : 'right'
      const next: Layout = {
        left: current.left.filter(id => id !== dragged),
        right: current.right.filter(id => id !== dragged),
      }
      const targetList = next[targetColumn]
      const insertion = targetList.indexOf(targetId)
      targetList.splice(insertion < 0 ? targetList.length : insertion, 0, dragged)
      if (sourceColumn === targetColumn) {
        const original = current[sourceColumn]
        const from = original.indexOf(dragged)
        const to = original.indexOf(targetId)
        const reordered = original.filter(id => id !== dragged)
        reordered.splice(Math.max(0, to - (from < to ? 1 : 0)), 0, dragged)
        next[sourceColumn] = reordered
      }
      localStorage.setItem('measure-dashboard-layout', JSON.stringify(next))
      return next
    })
    setDragged(null)
  }

  const submit = async (path: string, form: Record<string, string>) => {
    try {
      const response = await fetch(path, {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: new URLSearchParams(form),
      })
      const result = await response.json() as { message?: string; error?: string }
      if (!response.ok) throw new Error(result.error || `HTTP ${response.status}`)
      setNotice(result.message || 'Request completed.')
      await refresh()
    } catch (error) {
      setNotice(String(error))
    }
  }

  const connected = data.devices.filter(device => device.connected).length
  const renderPanel = (id: string) => {
    const dragProps = { onDragStart: setDragged, onDrop: movePanel }
    switch (id) {
      case 'configuration':
        return (
          <Panel key={id} id={id} title="Fleet Configuration" {...dragProps}>
            <div className="table-body config-table">
              <div><span>Devices</span><a href="#panel-devices">{data.devices.length} in fleet</a></div>
              <div><span>Online status</span><b>{connected} connected</b></div>
              <div><span>Measurement threshold</span><b>{data.threshold}</b></div>
            </div>
          </Panel>
        )
      case 'threshold':
        return (
          <Panel key={id} id={id} title="Threshold Configuration" {...dragProps}>
            <form className="panel-form" onSubmit={event => {
              event.preventDefault()
              void submit('/api/threshold', { threshold })
            }}>
              <label>Fleet threshold<input type="number" min="0" max="100000" required value={threshold} onChange={event => setThreshold(event.target.value)} /></label>
              <button type="submit">Apply threshold</button>
            </form>
            {notice && <div className="notice" role="status">{notice}</div>}
          </Panel>
        )
      case 'calibration':
        return (
          <Panel key={id} id={id} title="Calibration" {...dragProps}>
            <form className="panel-form" onSubmit={event => {
              event.preventDefault()
              void submit('/api/calibration', { client_id: target, duration_seconds: duration })
            }}>
              <label>Target device<select value={target} onChange={event => setTarget(event.target.value)}>
                <option value="">All connected devices</option>
                {data.devices.filter(device => device.connected).map(device =>
                  <option key={device.client_id} value={device.client_id}>{device.client_id}</option>)}
              </select></label>
              <label>Duration (seconds)<input type="number" min="1" max="3600" required value={duration} onChange={event => setDuration(event.target.value)} /></label>
              <button className="secondary" type="submit">Start calibration</button>
            </form>
          </Panel>
        )
      case 'devices':
        return (
          <Panel key={id} id={id} title="Fleet Devices" {...dragProps}>
            <div className="table-scroll">
              <table><thead><tr><th>Device</th><th>Status</th><th>Latest reading</th><th>Last seen</th></tr></thead>
                <tbody>{data.devices.map(device => (
                  <tr key={device.client_id}>
                    <td>{device.client_id}</td>
                    <td><span className="status"><i className={device.connected ? 'online' : ''} />{device.connected ? 'Online' : 'Offline'}</span></td>
                    <td>{device.latest_point ?? '—'}</td><td>{timeLabel(device.timestamp_unix_ms)}</td>
                  </tr>
                ))}</tbody>
              </table>
              {!data.devices.length && <div className="empty">No devices have connected or submitted measurements yet.</div>}
            </div>
          </Panel>
        )
      case 'chart':
        return (
          <Panel key={id} id={id} title="Fleet Measurement Chart" {...dragProps}>
            <div className="chart-content"><Chart events={data.events} /></div>
          </Panel>
        )
      case 'history':
        return (
          <Panel key={id} id={id} title="Measurement Event History" {...dragProps}>
            <div className="table-scroll history">
              <table><thead><tr><th>Time</th><th>Device</th><th>Measurement</th></tr></thead>
                <tbody>{data.events.slice().reverse().slice(0, 100).map((item, index) => (
                  <tr key={`${item.timestamp_unix_ms}-${item.client_id}-${index}`}>
                    <td>{timeLabel(item.timestamp_unix_ms)}</td><td>{item.client_id}</td><td>{item.point}</td>
                  </tr>
                ))}</tbody>
              </table>
              {!data.events.length && <div className="empty">No measurement events yet.</div>}
            </div>
          </Panel>
        )
      default:
        return null
    }
  }

  return (
    <div className="app-shell">
      <header className="topbar">
        <button className="menu-button" aria-label="Open navigation">☰</button>
        <div className="brand"><strong>Measure</strong><span>Online</span></div>
        <nav className="breadcrumbs" aria-label="Dashboard navigation">
          <span>▦　Fleet</span><b>›</b><span>▱　Devices</span><b>›</b><span>⌁　Activity</span><b>›</b><span>⚙　Operations</span>
        </nav>
        <div className="clock">        <strong>◷　{now ? new Date(now).toLocaleTimeString([], { timeZone: 'UTC', timeZoneName: 'short' }) : '— UTC'}</strong>
        <span>{now ? new Date(now).toLocaleTimeString() : '— Local'}</span></div>
        <button className="more-button" aria-label="More options">⋮</button>
      </header>
      <main className="dashboard-layout">
        <div className="dashboard-column left-column" onDragOver={event => event.preventDefault()}>
          {layout.left.map(renderPanel)}
        </div>
        <div className="dashboard-column right-column" onDragOver={event => event.preventDefault()}>
          {layout.right.map(renderPanel)}
        </div>
      </main>
    </div>
  )
}

export default App
