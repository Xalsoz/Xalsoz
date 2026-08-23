import { useEffect } from 'react'
import { useNavigate } from 'react-router-dom'
import { ScreenShell } from '../components/ScreenShell'
import { useTrainingFlow } from '../context/TrainingFlowContext'
import { ENERGY_LABELS } from '../lib/labels'
import type { EnergyLevel } from '../types'

const ORDER: EnergyLevel[] = ['low', 'medium', 'high']

const ENERGY_ICONS: Record<EnergyLevel, string> = {
  low: '🔋',
  medium: '🔋🔋',
  high: '🔋🔋🔋',
}

export function EnergySelect() {
  const navigate = useNavigate()
  const { environment, setEnergyLevel } = useTrainingFlow()

  useEffect(() => {
    if (!environment) navigate('/train/environment', { replace: true })
  }, [environment, navigate])

  function choose(level: EnergyLevel) {
    setEnergyLevel(level)
    navigate('/train/program')
  }

  return (
    <ScreenShell title="Сколько сил сегодня?" onBack={() => navigate('/train/environment')}>
      <div className="mt-4 flex flex-col gap-3">
        {ORDER.map((level) => (
          <button
            key={level}
            type="button"
            onClick={() => choose(level)}
            className="flex items-center gap-4 rounded-2xl border border-slate-800 bg-slate-900/60 px-5 py-5 text-left active:bg-slate-800"
          >
            <span className="text-2xl">{ENERGY_ICONS[level]}</span>
            <span className="text-lg font-medium text-slate-100">{ENERGY_LABELS[level]}</span>
          </button>
        ))}
      </div>
    </ScreenShell>
  )
}
