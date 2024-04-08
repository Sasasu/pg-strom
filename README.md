PG-Strom
========
PG-Strom is an extension for PostgreSQL database.
It is designed to accelerate mostly batch and analytics workloads with
utilization of GPU and NVME-SSD, and Apache Arrow columnar.

For more details, see the documentation below.
http://heterodb.github.io/pg-strom/

Software is fully open source, distributed under PostgreSQL License.

// QQQ http://heterodb.github.io/pg-strom/operations/

很小的项目，抛出优化部分后复杂度很低
适合 agg，输出数据要尽可能小。执行模型依然是 PostgreSQL 单条模型
开线程隔离 CPU 与 GPU，让 CPU 没有数据输出也可以继续输入数据给 GPU
GPU 执行器使用 codegen 加 CommandTag 来发送，没有图计算部分，并没有完全把计算交给 GPU。必要时仍使用 PostgreSQL 模型推动数据
