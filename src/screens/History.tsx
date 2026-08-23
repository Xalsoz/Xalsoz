import { useNavigate } from 'react-router-dom'
import { Card } from '../components/Card'
import { ScreenShell } from '../components/ScreenShell'
import { useAppData } from '../context/AppDataContext'
import { ENERGY_LABELS, ENVIRONMENT_LABELS, UNIT_LABELS } from '../lib/labels'

export function History() {
  const navigate = useNavigate()
  const { workoutLog } = useAppData()

  const sorted = [...workoutLog].sort((a, b) => b.date.localeCompare(a.date))

  return (
    <ScreenShell title="История тренировок" onBack={() => navigate('/')}>
      {sorted.length === 0 ? (
        <Card className="mt-6">
          <p className="text-sm text-slate-400">Пока пусто. Первая тренировка появится здесь.</p>
        </Card>
      ) : (
        <div className="mt-4 flex flex-col gap-3">
          {sorted.map((entry) => (
            <Card key={entry.id}>
              <div className="flex items-center justify-between">
                <span className="font-medium text-slate-100">
                  {new Date(entry.date).toLocaleDateString('ru-RU', {
                    day: 'numeric',
                    month: 'long',
                  })}
                </span>
                <span className="text-xs text-slate-400">
                  {ENVIRONMENT_LABELS[entry.environment]} · {ENERGY_LABELS[entry.energyLevel]}
                </span>
              </div>
              <ul className="mt-3 space-y-1">
                {entry.exercises.map((ex) => (
                  <li key={ex.exerciseId} className="flex justify-between text-sm">
                    <span className="text-slate-300">{ex.name}</span>
                    <span className="text-slate-500">
                      {ex.actual != null
                        ? `${ex.actual} ${UNIT_LABELS[ex.unit]} (план ${ex.sets}×${ex.target})`
                        : `план ${ex.sets}×${ex.target} ${UNIT_LABELS[ex.unit]}`}
                    </span>
                  </li>
                ))}
              </ul>
            </Card>
          ))}
        </div>
      )}
    </ScreenShell>
  )
}
