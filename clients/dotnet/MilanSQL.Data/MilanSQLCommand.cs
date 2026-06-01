using System;
using System.Data;
using System.Data.Common;

namespace MilanSQL.Data
{
    public class MilanSQLCommand : DbCommand
    {
        private MilanSQLConnection? _conn;
        private string _cmdText = "";
        private MilanSQLParameterCollection _params = new();

        public MilanSQLCommand() {}
        public MilanSQLCommand(MilanSQLConnection conn) { _conn = conn; }
        public MilanSQLCommand(string sql, MilanSQLConnection conn) { _cmdText = sql; _conn = conn; }

        public override string CommandText { get => _cmdText; set => _cmdText = value; }
        public override int CommandTimeout { get; set; } = 30;
        public override CommandType CommandType { get; set; } = CommandType.Text;
        public override bool DesignTimeVisible { get; set; }
        public override UpdateRowSource UpdatedRowSource { get; set; }

        protected override DbConnection? DbConnection { get => _conn; set => _conn = value as MilanSQLConnection; }
        protected override DbParameterCollection DbParameterCollection => _params;
        protected override DbTransaction? DbTransaction { get; set; }

        private string BuildSql()
        {
            string sql = _cmdText;
            foreach (MilanSQLParameter p in _params)
            {
                string val = p.Value == null ? "NULL" :
                    p.Value is string s ? $"'{s.Replace("'", "''")}'" :
                    p.Value.ToString()!;
                sql = sql.Replace(p.ParameterName, val);
            }
            return sql;
        }

        public override void Cancel() {}
        public override void Prepare() {}

        protected override DbParameter CreateDbParameter() => new MilanSQLParameter();

        public override int ExecuteNonQuery()
        {
            _conn!.SendQuery(BuildSql());
            return 1;
        }

        public override object? ExecuteScalar()
        {
            var lines = _conn!.SendQuery(BuildSql());
            var rs = MilanSQLDataReader.Parse(lines, this);
            if (rs.Read()) return rs.GetValue(0);
            return null;
        }

        protected override DbDataReader ExecuteDbDataReader(CommandBehavior behavior)
        {
            var lines = _conn!.SendQuery(BuildSql());
            return MilanSQLDataReader.Parse(lines, this);
        }
    }
}
