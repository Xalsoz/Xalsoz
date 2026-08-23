import { useNavigate } from 'react-router-dom'
import { ScreenShell } from '../components/ScreenShell'
import { useTrainingFlow } from '../context/TrainingFlowContext'
import { ENVIRONMENT_ICONS, ENVIRONMENT_LABELS } from '../lib/labels'
import type { Environment } from '../types'

const ORDER: Environment[] = ['home-no-bar', 'home-bar', 'park', 'gym', 'no-equipment']

export function EnvironmentSelect() {
  const navigate = useNavigate()
  const { setEnvironment } = useTrainingFlow()

  function choose(env: Environment) {
    setEnvironment(env)
    navigate('/train/energy')
  }

  return (
    <ScreenShell title="Где занимаемся?" onBack={() => navigate('/')}>
      <div className="mt-4 grid grid-cols-1 gap-3">
        {ORDER.map((env) => (
          <button
            key={env}
            type="button"
            onClick={() => choose(env)}
            className="flex items-center gap-4 rounded-2xl border border-slate-800 bg-slate-900/60 px-5 py-4 text-left active:bg-slate-800"
          >
            <span className="text-3xl">{ENVIRONMENT_ICONS[env]}</span>
            <span className="text-base font-medium text-slate-100">{ENVIRONMENT_LABELS[env]}</span>
          </button>
        ))}
      </div>
    </ScreenShell>
  )
}
