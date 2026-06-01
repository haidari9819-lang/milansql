using System;
using System.Data;
using System.Data.Common;

namespace MilanSQL.Data
{
    public class MilanSQLParameter : DbParameter
    {
        public override DbType DbType { get; set; } = DbType.String;
        public override ParameterDirection Direction { get; set; } = ParameterDirection.Input;
        public override bool IsNullable { get; set; }
        public override string ParameterName { get; set; } = "";
        public override int Size { get; set; }
        public override string SourceColumn { get; set; } = "";
        public override bool SourceColumnNullMapping { get; set; }
        public override object? Value { get; set; }
        public override void ResetDbType() { DbType = DbType.String; }
    }

    public class MilanSQLParameterCollection : DbParameterCollection
    {
        private readonly System.Collections.Generic.List<MilanSQLParameter> _list = new();
        public override int Count => _list.Count;
        public override object SyncRoot => this;
        public override int Add(object value) { _list.Add((MilanSQLParameter)value); return _list.Count - 1; }
        public MilanSQLParameter AddWithValue(string name, object value) {
            var p = new MilanSQLParameter { ParameterName = name, Value = value };
            _list.Add(p); return p;
        }
        public override void AddRange(Array values) { foreach (var v in values) Add(v); }
        public override void Clear() => _list.Clear();
        public override bool Contains(object value) => _list.Contains((MilanSQLParameter)value);
        public override bool Contains(string value) => _list.Exists(p => p.ParameterName == value);
        public override void CopyTo(Array array, int index) => ((System.Collections.IList)_list).CopyTo(array, index);
        public override System.Collections.IEnumerator GetEnumerator() => _list.GetEnumerator();
        protected override DbParameter GetParameter(int index) => _list[index];
        protected override DbParameter GetParameter(string name) => _list.Find(p => p.ParameterName == name)!;
        public override int IndexOf(object value) => _list.IndexOf((MilanSQLParameter)value);
        public override int IndexOf(string parameterName) => _list.FindIndex(p => p.ParameterName == parameterName);
        public override void Insert(int index, object value) => _list.Insert(index, (MilanSQLParameter)value);
        public override void Remove(object value) => _list.Remove((MilanSQLParameter)value);
        public override void RemoveAt(int index) => _list.RemoveAt(index);
        public override void RemoveAt(string parameterName) => _list.RemoveAll(p => p.ParameterName == parameterName);
        protected override void SetParameter(int index, DbParameter value) => _list[index] = (MilanSQLParameter)value;
        protected override void SetParameter(string parameterName, DbParameter value) {
            int i = IndexOf(parameterName); if (i >= 0) _list[i] = (MilanSQLParameter)value;
        }
    }

    public class MilanSQLTransaction : System.Data.Common.DbTransaction
    {
        private readonly MilanSQLConnection _conn;
        private readonly IsolationLevel _level;
        public MilanSQLTransaction(MilanSQLConnection conn, IsolationLevel level) { _conn = conn; _level = level; }
        protected override DbConnection DbConnection => _conn;
        public override IsolationLevel IsolationLevel => _level;
        public override void Commit() => _conn.SendQuery("COMMIT");
        public override void Rollback() => _conn.SendQuery("ROLLBACK");
    }
}
