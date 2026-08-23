import { useNavigate } from 'react-router-dom'
import { Card } from '../components/Card'
import { ScreenShell } from '../components/ScreenShell'
import { useAppData } from '../context/AppDataContext'
import {
  formatDelta,
  getMovementHistory,
  getTwoWeekSummary,
  TRACKED_MOVEMENT_LABELS,
} from '../lib/progress'
import type { TrackedMovement } from '../types'

const MOVEMENTS: TrackedMovement[] = ['pullUps', 'pushUps', 'dips', 'squats', 'plank']

function Sparkline({ values }: { values: number[] }) {
  if (values.length < 2) return null
  const w = 280
  const h = 48
  const max = Math.max(...values)
  const min = Math.min(...values)
  const range = max - min || 1
  const step = w / (values.length - 1)
  const points = values
    .map((v, i) => `${i * step},${h - ((v - min) / range) * (h - 8) - 4}`)
    .join(' ')

  return (
    <svg viewBox={`0 0 ${w} ${h}`} className="mt-2 w-full text-emerald-400">
      <polyline points={points} fill="none" stroke="currentColor" strokeWidth={2} />
    </svg>
  )
}

export function Progress() {
  const navigate = useNavigate()
  const { workoutLog } = useAppData()
  const summary = getTwoWeekSummary(workoutLog)

  return (
    <ScreenShell title="Прогресс" onBack={() => navigate('/')}>
      <section className="mt-4">
        <h2 className="text-sm font-medium text-slate-400">Было / стало за 2 недели</h2>
        {summary.length === 0 ? (
          <Card className="mt-3">
            <p className="text-sm text-slate-400">Пока недостаточно данных.</p>
          </Card>
        ) : (
          <div className="mt-3 space-y-2">
            {summary.map((s) => (
              <Card key={s.movement} className="flex items-center justify-between">
                <span className="text-slate-300">{TRACKED_MOVEMENT_LABELS[s.movement]}</span>
                <span className="font-semibold text-emerald-400">
                  {formatDelta(s.before, s.after)}
                </span>
              </Card>
            ))}
          </div>
        )}
      </section>

      <section className="mt-8">
        <h2 className="text-sm font-medium text-slate-400">Динамика по упражнениям</h2>
        <div className="mt-3 space-y-3">
          {MOVEMENTS.map((movement) => {
            const history = getMovementHistory(workoutLog, movement)
            if (history.length === 0) return null
            return (
              <Card key={movement}>
                <div className="flex items-center justify-between">
                  <h3 className="font-medium text-slate-100">
                    {TRACKED_MOVEMENT_LABELS[movement]}
                  </h3>
                  <span className="text-sm text-slate-400">
                    {history.length} {history.length === 1 ? 'запись' : 'записей'}
                  </span>
                </div>
                <Sparkline values={history.map((p) => p.value)} />
                <div className="mt-1 flex justify-between text-xs text-slate-500">
                  <span>{new Date(history[0].date).toLocaleDateString('ru-RU')}</span>
                  <span>{new Date(history[history.length - 1].date).toLocaleDateString('ru-RU')}</span>
                </div>
              </Card>
            )
          })}
          {MOVEMENTS.every((m) => getMovementHistory(workoutLog, m).length === 0) && (
            <Card>
              <p className="text-sm text-slate-400">
                Пока нет данных. Заверши тренировку и внеси результат — здесь появится график.
              </p>
            </Card>
          )}
        </div>
      </section>
    </ScreenShell>
  )
}
