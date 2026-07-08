import React from "react";

interface MilanSQLLogoProps {
  size?: number;
  className?: string;
}

/**
 * MilanSQL Graph-Node-M Logo
 * Scalable SVG component — pass `size` for width/height in px.
 */
export const MilanSQLLogo: React.FC<MilanSQLLogoProps> = ({
  size = 40,
  className,
}) => (
  <svg
    width={size}
    height={size}
    viewBox="0 0 100 100"
    xmlns="http://www.w3.org/2000/svg"
    className={className}
  >
    <rect x="0" y="0" width="100" height="100" rx="8" fill="#161616" stroke="#ff6b1a" strokeWidth="0.5" />
    <path
      d="M20 78 L20 22 L50 54 L80 22 L80 78"
      fill="none"
      stroke="#ff6b1a"
      strokeWidth="2.5"
      strokeLinecap="round"
      strokeLinejoin="round"
    />
    <circle cx="20" cy="22" r="5" fill="#ff6b1a" />
    <circle cx="20" cy="78" r="5" fill="#ff6b1a" />
    <circle cx="50" cy="54" r="5" fill="#ff6b1a" />
    <circle cx="80" cy="22" r="5" fill="#ff6b1a" />
    <circle cx="80" cy="78" r="5" fill="#ff6b1a" />
  </svg>
);

export default MilanSQLLogo;
