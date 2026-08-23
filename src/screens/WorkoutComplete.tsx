import { useEffect, useState } from 'react'
import { useNavigate } from 'react-router-dom'
import { Button } from '../components/Button'
import { Card } from '../components/Card'
import { ScreenShell } from '../components/ScreenShell'
import { useAppData } from '../context/AppDataContext'
import { useTrainingFlow } from '../context/TrainingFlowContext'
import { UNIT_LABELS } from '../lib/labels'
import type { LoggedExercise, WorkoutLogEntry } from '../types'

export function WorkoutComplete() {
  const navigate = useNavigate()
  const { environment, energyLevel, program, reset } = useTrainingFlow()
  const { appendWorkoutLog } = useAppData()

  const [actuals, setActuals] = useState<Record<string, string>>({})
  const [saving, setSaving] = useState(false)

  useEffect(() => {
    if (!environment || !energyLevel || !program) {
      navigate('/', { replace: true })
    }
  }, [environment, energyLevel, program, navigate])

  if (!environment || !energyLevel || !program) {
    return null
  }

  async function save() {
    setSaving(true)
    const exercises: LoggedExercise[] = program!.exercises.map((ex) => {
      const raw = actuals[ex.exerciseId]
      const actual = raw !== undefined && raw.trim() !== '' ? Math.max(0, Math.round(Number(raw))) : null
      return {
        exerciseId: ex.exerciseId,
        name: ex.name,
        tracks: ex.tracks,
        unit: ex.unit,
        sets: ex.sets,
        target: ex.target,
        actual,
      }
    })

    const entry: WorkoutLogEntry = {
      id: crypto.randomUUID(),
      date: new Date().toISOString(),
      environment: environment!,
      energyLevel: energyLevel!,
      exercises,
    }

    await appendWorkoutLog(entry)
    reset()
    navigate('/', { replace: true })
  }

  function skip() {
    reset()
    navigate('/', { replace: true })
  }

  return (
    <ScreenShell title="Что реально получилось?">
      <p className="mt-1 text-sm text-slate-400">
        Введи фактическое число — по каждому упражнению, которое хочешь запомнить.
      </p>

      <div className="mt-5 flex flex-col gap-3">
        {program.exercises.map((ex) => (
          <Card key={ex.exerciseId}>
            <div className="flex items-center justify-between gap-3">
              <div>
                <h3 className="text-base font-semibold text-slate-100">{ex.name}</h3>
                <p className="text-xs text-slate-500">
                  план: {ex.sets} × {ex.target} {UNIT_LABELS[ex.unit]}
                </p>
              </div>
              <input
                type="number"
                inputMode="numeric"
                min={0}
                placeholder="0"
                value={actuals[ex.exerciseId] ?? ''}
                onChange={(e) =>
                  setActuals((prev) => ({ ...prev, [ex.exerciseId]: e.target.value }))
                }
                className="w-20 rounded-xl border border-slate-700 bg-slate-900 px-3 py-2 text-center text-lg text-slate-100 outline-none focus:border-emerald-500"
              />
            </div>
          </Card>
        ))}
      </div>

      <div className="mt-8 flex flex-col gap-3">
        <Button onClick={save} disabled={saving}>
          {saving ? 'Сохраняем…' : 'Сохранить'}
        </Button>
        <Button variant="ghost" onClick={skip}>
          Пропустить
        </Button>
      </div>
    </ScreenShell>
  )
}
