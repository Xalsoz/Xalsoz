import { Link, useNavigate } from 'react-router-dom'
import { Button } from '../components/Button'
import { Card } from '../components/Card'
import { useAppData } from '../context/AppDataContext'
import { useTrainingFlow } from '../context/TrainingFlowContext'
import { formatDelta, getTwoWeekSummary, hasLowEnergyStreak, TRACKED_MOVEMENT_LABELS } from '../lib/progress'

export function Home() {
  const navigate = useNavigate()
  const { workoutLog } = useAppData()
  const { reset } = useTrainingFlow()

  const summary = getTwoWeekSummary(workoutLog)
  const showLowEnergyNote = hasLowEnergyStreak(workoutLog)
  const lastSession = [...workoutLog].sort((a, b) => b.date.localeCompare(a.date))[0]

  function startTraining() {
    reset()
    navigate('/train/environment')
  }

  return (
    <div className="flex min-h-dvh flex-col px-5 pt-10 pb-6">
      <h1 className="text-xl font-bold text-slate-100">Калистеника без расписания</h1>
      <p className="mt-1 text-sm text-slate-400">
        {lastSession
          ? `Последняя тренировка: ${new Date(lastSession.date).toLocaleDateString('ru-RU')}`
          : 'Пока не было ни одной тренировки'}
      </p>

      <div className="mt-10 flex flex-col items-center">
        <button
          type="button"
          onClick={startTraining}
          className="flex h-52 w-52 items-center justify-center rounded-full bg-emerald-500 text-center text-2xl font-bold text-slate-950 shadow-2xl shadow-emerald-500/30 active:scale-95 transition-transform"
        >
          Хочу
          <br />
          позаниматься
        </button>
      </div>

      {showLowEnergyNote && (
        <Card className="mt-8 border-amber-800/60 bg-amber-950/30">
          <p className="text-sm text-amber-200">
            Смотрим, что в последнее время маловато сил — это нормально, просто дай знать себе,
            если что-то не так.
          </p>
        </Card>
      )}

      <div className="mt-8">
        <h2 className="text-sm font-medium text-slate-400">Прогресс за 2 недели</h2>
        {summary.length === 0 ? (
          <Card className="mt-3">
            <p className="text-sm text-slate-400">
              Пока недостаточно данных. Позанимайся пару раз — здесь появятся цифры.
            </p>
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
      </div>

      <div className="mt-auto pt-8 flex flex-col gap-3">
        <Link to="/progress">
          <Button variant="secondary">Прогресс</Button>
        </Link>
        <Link to="/history">
          <Button variant="ghost">История тренировок</Button>
        </Link>
      </div>
    </div>
  )
}
