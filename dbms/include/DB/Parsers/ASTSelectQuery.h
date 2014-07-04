#pragma once

#include <DB/Parsers/IAST.h>
#include <DB/Parsers/ASTQueryWithOutput.h>
#include <DB/Parsers/ASTExpressionList.h>
#include <DB/Parsers/ASTFunction.h>

namespace DB
{


/** SELECT запрос
  */
class ASTSelectQuery : public ASTQueryWithOutput
{
public:
	bool distinct = false;
	ASTPtr select_expression_list;
	ASTPtr database;
	ASTPtr table;	/// Идентификатор, табличная функция или подзапрос (рекурсивно ASTSelectQuery)
	ASTPtr array_join_expression_list;	/// ARRAY JOIN
	ASTPtr join;						/// Обычный (не ARRAY) JOIN.
	bool final = false;
	ASTPtr sample_size;
	ASTPtr prewhere_expression;
	ASTPtr where_expression;
	ASTPtr group_expression_list;
	bool group_by_with_totals = false;
	ASTPtr having_expression;
	ASTPtr order_expression_list;
	ASTPtr limit_offset;
	ASTPtr limit_length;

	ASTSelectQuery() {}
	ASTSelectQuery(StringRange range_) : ASTQueryWithOutput(range_) {}

	/** Получить текст, который идентифицирует этот элемент. */
	String getID() const { return "SelectQuery"; };

	/// Проверить наличие функции arrayJoin. (Не большого ARRAY JOIN.)
	static bool hasArrayJoin(const ASTPtr & ast)
	{
		if (const ASTFunction * function = typeid_cast<const ASTFunction *>(&*ast))
		{
			if (function->kind == ASTFunction::ARRAY_JOIN)
				return true;
		}
		for (const auto & child : ast->children)
			if (hasArrayJoin(child))
				return true;
		return false;
	}

	/// Переписывает select_expression_list, чтобы вернуть только необходимые столбцы в правильном порядке.
	void rewriteSelectExpressionList(const Names & column_names)
	{
		ASTPtr result = new ASTExpressionList;
		ASTs asts = select_expression_list->children;

		/// Не будем выбрасывать выражения, содержащие функцию arrayJoin.
		std::set<ASTPtr> unremovable_asts;
		for (size_t j = 0; j < asts.size(); ++j)
		{
			if (hasArrayJoin(asts[j]))
			{
				result->children.push_back(asts[j]->clone());
				unremovable_asts.insert(asts[j]);
			}
		}

		for (size_t i = 0; i < column_names.size(); ++i)
		{
			bool done = 0;
			for (size_t j = 0; j < asts.size(); ++j)
			{
				if (asts[j]->getAliasOrColumnName() == column_names[i])
				{
					if (!unremovable_asts.count(asts[j]))
						result->children.push_back(asts[j]->clone());
					done = 1;
				}
			}
			if (!done)
				throw Exception("Error while rewriting expression list for select query."
					" Could not find alias: " + column_names[i],
					DB::ErrorCodes::UNKNOWN_IDENTIFIER);
		}

		for (auto & child : children)
		{
			if (child == select_expression_list)
			{
				child = result;
				break;
			}
		}
		select_expression_list = result;

		/** NOTE: Может показаться, что мы могли испортить запрос, выбросив выражение с алиасом, который используется где-то еще.
		  *       Такого произойти не может, потому что этот метод вызывается всегда для запроса, на котором хоть раз создавали
		  *       ExpressionAnalyzer, что гарантирует, что в нем все алиасы уже подставлены. Не совсем очевидная логика :)
		  */
	}

	ASTPtr clone() const
	{
		ASTSelectQuery * res = new ASTSelectQuery(*this);
		res->children.clear();

#define CLONE(member) if (member) { res->member = member->clone(); res->children.push_back(res->member); }

		CLONE(select_expression_list)
		CLONE(database)
		CLONE(table)
		CLONE(array_join_expression_list)
		CLONE(join)
		CLONE(sample_size)
		CLONE(prewhere_expression)
		CLONE(where_expression)
		CLONE(group_expression_list)
		CLONE(having_expression)
		CLONE(order_expression_list)
		CLONE(limit_offset)
		CLONE(limit_length)
		CLONE(format)

#undef CLONE

		return res;
	}
};

}
