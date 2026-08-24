import { useEffect, useRef } from 'react'
import { useNavigate } from 'react-router-dom'
import { Button } from '../components/Button'
import { Card } from '../components/Card'
import { ScreenShell } from '../components/ScreenShell'
import { useAppData } from '../context/AppDataContext'
import { useTrainingFlow } from '../context/TrainingFlowContext'
import { generateProgram, regenerateExercise } from '../lib/programGenerator'
import { ENERGY_LABELS, ENVIRONMENT_LABELS, UNIT_LABELS } from '../lib/labels'

export function WorkoutProgram() {
  const navigate = useNavigate()
  const { environment, energyLevel, program, setProgram } = useTrainingFlow()
  const {
    userProfile,
    workoutLog,
    programHistory,
    progressionState,
    appendProgramRecord,
    saveProgressionState,
  } = useAppData()

  const generatedRef = useRef(false)

  useEffect(() => {
    if (!environment || !energyLevel) {
      navigate('/train/environment', { replace: true })
      return
    }
    if (!userProfile) {
      navigate('/onboarding', { replace: true })
      return
    }
    if (generatedRef.current || program) return
    generatedRef.current = true

    const { program: generated, nextProgressionState } = generateProgram({
      environment,
      energyLevel,
      profile: userProfile,
      workoutLog,
      programHistory,
      progressionState,
    })

    setProgram(generated)
    void appendProgramRecord({
      comboKey: generated.comboKey,
      exerciseIds: generated.exercises.map((e) => e.exerciseId),
      generatedAt: new Date().toISOString(),
    })
    void saveProgressionState(nextProgressionState)
    // Runs once per flow entry — deps intentionally limited to the guard values.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [environment, energyLevel, userProfile])

  if (!environment || !energyLevel || !program) {
    return (
      <ScreenShell title="Собираем программу…">
        <p className="mt-6 text-slate-400">Секунду…</p>
      </ScreenShell>
    )
  }

  function swapExercise(index: number) {
    if (!environment || !energyLevel || !program || !userProfile) return
    const target = program.exercises[index]
    const replacement = regenerateExercise({
      muscleGroup: target.muscleGroup,
      environment,
      energyLevel,
      profile: userProfile,
      workoutLog,
      programHistory,
      progressionState,
      excludeIds: program.exercises.map((e) => e.exerciseId),
    })
    if (!replacement) return
    const exercises = [...program.exercises]
    exercises[index] = replacement
    setProgram({ ...program, exercises })
  }

  return (
    <ScreenShell title="Твоя программа" onBack={() => navigate('/train/energy')}>
      <p className="mt-1 text-sm text-slate-400">
        {ENVIRONMENT_LABELS[environment]} · энергия: {ENERGY_LABELS[energyLevel].toLowerCase()}
      </p>

      <div className="mt-5 flex flex-col gap-3">
        {program.exercises.map((ex, i) => (
          <Card key={ex.exerciseId}>
            <div className="flex items-start justify-between gap-3">
              <div>
                <span className="text-xs text-slate-500">#{i + 1}</span>
                <h3 className="text-base font-semibold text-slate-100">{ex.name}</h3>
                {ex.variant && (
                  <p className="text-xs text-emerald-400">{ex.variant}</p>
                )}
              </div>
              <div className="whitespace-nowrap text-right text-sm font-semibold text-slate-200">
                {ex.sets} × {ex.target} {UNIT_LABELS[ex.unit]}
              </div>
            </div>
            <p className="mt-2 text-sm text-slate-400">{ex.description}</p>
            <button
              type="button"
              onClick={() => swapExercise(i)}
              className="mt-3 text-xs font-medium text-emerald-400 active:text-emerald-300"
            >
              ⟲ Заменить
            </button>
          </Card>
        ))}
      </div>

      <div className="mt-8">
        <Button onClick={() => navigate('/train/complete')}>Тренировка завершена</Button>
      </div>
    </ScreenShell>
  )
}
