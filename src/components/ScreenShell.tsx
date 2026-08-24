import type { ReactNode } from 'react'
import { useNavigate } from 'react-router-dom'

interface ScreenShellProps {
  title?: string
  onBack?: () => void
  children: ReactNode
  footer?: ReactNode
}

export function ScreenShell({ title, onBack, children, footer }: ScreenShellProps) {
  const navigate = useNavigate()

  return (
    <div className="flex min-h-dvh flex-col">
      {(title || onBack) && (
        <header className="safe-top flex items-center gap-3 px-4 pt-4 pb-2">
          {onBack && (
            <button
              type="button"
              onClick={onBack ?? (() => navigate(-1))}
              aria-label="Назад"
              className="flex h-9 w-9 items-center justify-center rounded-full bg-slate-800 text-slate-300 active:bg-slate-700"
            >
              ←
            </button>
          )}
          {title && <h1 className="text-lg font-semibold text-slate-100">{title}</h1>}
        </header>
      )}
      <main className="flex flex-1 flex-col px-4 pb-4">{children}</main>
      {footer && <footer className="safe-bottom px-4 pb-4">{footer}</footer>}
    </div>
  )
}
