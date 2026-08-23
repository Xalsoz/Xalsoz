import { useState } from 'react'
import { useNavigate } from 'react-router-dom'
import { Button } from '../components/Button'
import { useAppData } from '../context/AppDataContext'
import type { UserProfile } from '../types'

type Step = 0 | 1 | 2

export function Onboarding() {
  const navigate = useNavigate()
  const { saveUserProfile } = useAppData()

  const [step, setStep] = useState<Step>(0)
  const [pullUps, setPullUps] = useState('')
  const [pushUps, setPushUps] = useState('')
  const [restrictions, setRestrictions] = useState('')
  const [weeklyFrequencyGoal, setWeeklyFrequencyGoal] = useState('3')
  const [saving, setSaving] = useState(false)

  const levelValid = pullUps.trim() !== '' && pushUps.trim() !== ''
  const frequencyValid =
    weeklyFrequencyGoal.trim() !== '' && Number(weeklyFrequencyGoal) > 0

  async function finish() {
    setSaving(true)
    const profile: UserProfile = {
      startingLevel: {
        pullUps: Math.max(0, Math.round(Number(pullUps) || 0)),
        pushUps: Math.max(0, Math.round(Number(pushUps) || 0)),
      },
      restrictions: restrictions.trim(),
      weeklyFrequencyGoal: Math.max(1, Math.round(Number(weeklyFrequencyGoal) || 1)),
      createdAt: new Date().toISOString(),
    }
    await saveUserProfile(profile)
    navigate('/', { replace: true })
  }

  return (
    <div className="flex min-h-dvh flex-col px-5 pt-10 pb-6">
      <div className="mb-8 flex justify-center gap-2">
        {[0, 1, 2].map((i) => (
          <div
            key={i}
            className={`h-1.5 w-8 rounded-full ${i <= step ? 'bg-emerald-500' : 'bg-slate-800'}`}
          />
        ))}
      </div>

      <div className="flex flex-1 flex-col">
        {step === 0 && (
          <div className="flex flex-1 flex-col">
            <h1 className="text-2xl font-bold text-slate-100">Какой у тебя уровень сейчас?</h1>
            <p className="mt-2 text-slate-400">
              Честно, без прикрас. 0 — тоже нормальный ответ.
            </p>

            <label className="mt-8 block text-sm text-slate-400">
              Сколько подтягиваний реально сделаешь подряд?
            </label>
            <input
              type="number"
              inputMode="numeric"
              min={0}
              value={pullUps}
              onChange={(e) => setPullUps(e.target.value)}
              placeholder="0"
              className="mt-2 w-full rounded-xl border border-slate-700 bg-slate-900 px-4 py-3 text-lg text-slate-100 outline-none focus:border-emerald-500"
            />

            <label className="mt-6 block text-sm text-slate-400">
              Сколько отжиманий реально сделаешь подряд?
            </label>
            <input
              type="number"
              inputMode="numeric"
              min={0}
              value={pushUps}
              onChange={(e) => setPushUps(e.target.value)}
              placeholder="0"
              className="mt-2 w-full rounded-xl border border-slate-700 bg-slate-900 px-4 py-3 text-lg text-slate-100 outline-none focus:border-emerald-500"
            />

            <div className="flex-1" />
            <Button disabled={!levelValid} onClick={() => setStep(1)}>
              Дальше
            </Button>
          </div>
        )}

        {step === 1 && (
          <div className="flex flex-1 flex-col">
            <h1 className="text-2xl font-bold text-slate-100">
              Есть травмы или ограничения?
            </h1>
            <p className="mt-2 text-slate-400">Необязательно. Можно пропустить.</p>

            <textarea
              value={restrictions}
              onChange={(e) => setRestrictions(e.target.value)}
              placeholder="Например: болит плечо, не делать глубокие приседания…"
              rows={5}
              className="mt-8 w-full resize-none rounded-xl border border-slate-700 bg-slate-900 px-4 py-3 text-base text-slate-100 outline-none focus:border-emerald-500"
            />

            <div className="flex-1" />
            <div className="flex gap-3">
              <Button variant="secondary" onClick={() => setStep(0)}>
                Назад
              </Button>
              <Button onClick={() => setStep(2)}>Дальше</Button>
            </div>
          </div>
        )}

        {step === 2 && (
          <div className="flex flex-1 flex-col">
            <h1 className="text-2xl font-bold text-slate-100">
              Сколько раз в неделю готов заниматься?
            </h1>
            <p className="mt-2 text-slate-400">
              Просто число. Дни выбирать не нужно — решишь сам, когда захочется.
            </p>

            <input
              type="number"
              inputMode="numeric"
              min={1}
              max={14}
              value={weeklyFrequencyGoal}
              onChange={(e) => setWeeklyFrequencyGoal(e.target.value)}
              className="mt-8 w-full rounded-xl border border-slate-700 bg-slate-900 px-4 py-3 text-lg text-slate-100 outline-none focus:border-emerald-500"
            />

            <div className="flex-1" />
            <div className="flex gap-3">
              <Button variant="secondary" onClick={() => setStep(1)}>
                Назад
              </Button>
              <Button disabled={!frequencyValid || saving} onClick={finish}>
                {saving ? 'Сохраняем…' : 'Готово'}
              </Button>
            </div>
          </div>
        )}
      </div>
    </div>
  )
}
