#include <memory>
#include <string>

#include <QTest>

#include <qkvm/base/result.h>

using qkvm::base::Error;
using qkvm::base::Result;

namespace {

Result<int> half(int number)
{
    if (number % 2 != 0) {
        return Error{"odd", "An odd number has no half."};
    }
    return number / 2;
}

Result<void> mustBeSmall(int number)
{
    if (number > 9) {
        return Error{"too_large", "That is more than nine."};
    }
    return {};
}

} // namespace

class TestResult : public QObject {
    Q_OBJECT

private slots:
    void aSuccessCarriesItsValue()
    {
        const Result<int> result = half(8);
        QVERIFY(result.ok());
        QVERIFY(static_cast<bool>(result));
        QCOMPARE(result.value(), 4);
    }

    void aFailureCarriesItsCodeAndItsSentence()
    {
        const Result<int> result = half(7);
        QVERIFY(!result.ok());
        QCOMPARE(result.error().code, std::string("odd"));
        QCOMPARE(result.error().message, std::string("An odd number has no half."));
    }

    void aValueThatCannotBeCopiedIsMovedOut()
    {
        Result<std::unique_ptr<int>> result = std::make_unique<int>(5);
        QVERIFY(result.ok());
        const std::unique_ptr<int> taken = std::move(result).value();
        QCOMPARE(*taken, 5);
    }

    void nothingToGiveBackIsASuccessToo()
    {
        QVERIFY(mustBeSmall(3).ok());
        const Result<void> result = mustBeSmall(12);
        QVERIFY(!result.ok());
        QCOMPARE(result.error().code, std::string("too_large"));
    }
};

QTEST_APPLESS_MAIN(TestResult)
#include "test_result.moc"
