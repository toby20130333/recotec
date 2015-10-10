#include "redismapprivate.h"

RedisMapPrivate::RedisMapPrivate(QString list, QString connectionName)
{
    this->redisList = list.toLocal8Bit();
    this->connectionName = connectionName;
}

void RedisMapPrivate::clear(bool async)
{
    // Build and execute Command
    // There is currently no implementation of HDEL MYLIST so we executing a script on the server doing the job
    // src: http://redis.io/commands/del#comment-1006084933 (adapted to hash)
    QByteArray res;
    RedisMapPrivate::execRedisCommand({"eval", "for _,k in ipairs(redis.call('HKEYS','" + this->redisList + "')) do redis.call('HDEL', '" + this->redisList + "', k) end", "0" }, async ? 0 : &res);
}

bool RedisMapPrivate::insert(QByteArray key, QByteArray value, bool waitForAnswer)
{
    // Build and execute Command
    // HSET list key value
    // src: http://redis.io/commands/HSET
    QByteArray returnValue;
    bool result = RedisMapPrivate::execRedisCommand({ "HSET", this->redisList, key, value }, waitForAnswer ? &returnValue : 0);

    // determinate result
    if(!waitForAnswer) return result;
    else return result && returnValue == "OK";
}

QByteArray RedisMapPrivate::value(QByteArray key)
{
    // Build and execute Command
    // HGET list key
    // src: http://redis.io/commands/HGET
    QByteArray returnValue;
    RedisMapPrivate::execRedisCommand({ "HGET", this->redisList, key }, &returnValue);

    // return result
    return returnValue;
}

bool RedisMapPrivate::execRedisCommand(std::initializer_list<QByteArray> cmd, QByteArray* result, QList<QByteArray*>* lstResultArray1, QList<QByteArray*>* lstResultArray2)
{
    // acquire socket
    bool waitForAnswer = result || lstResultArray1 || lstResultArray2;
    RedisConnectionReleaser con = RedisConnectionManager::requestConnection(this->connectionName, !waitForAnswer);
    if(!con.data() || !con->socket) return false;

    /// Build and execute RESP request
    /// see: http://redis.io/topics/protocol#resp-arrays
    // 1. determine allocation size
    int size = 15;
    for(auto itr = cmd.begin(); itr != cmd.end(); itr++) size += 15 + itr->length();

    // 2. build RESP request
    char* content = (char*)malloc(size);
    char* stringData = content;
    content += sprintf(content, "*%i\r\n", cmd.size());
    for(auto itr = cmd.begin(); itr != cmd.end(); itr++) {
        content += sprintf(content, "$%i\r\n", itr->isEmpty() ? -1 : itr->length());
        content = (char*)mempcpy(content, itr->data(), itr->length());
        *content = '\r';
        *++content = '\n';
        content++;
    }

    // 3. exec RESP request
    con->socket->write(stringData, content - stringData);
    free(stringData);

    // exit if we don't have to parse the return code
    if(!waitForAnswer) return true;

    // 4. wait for server return code
    con->socket->waitForReadyRead();
    QByteArray data = con->socket->readAll();

    /// Parse RESP Response
    /// see: http://redis.io/topics/protocol#resp-protocol-description

    // simplify variables
    char* rawData = data.data();
    int rawLength = data.length();
    char* respDataType = rawData++;

    // handle Simple String
    if(result && *respDataType == '+') {
        *result = QByteArray(rawData, rawLength - 2);
        return true;
    }

    // handle Error
    if(result && *respDataType == '-') {
        *result = QByteArray(rawData, rawLength - 2);
        return false;
    }

    // handle Integer
    if(result && *respDataType == ':') {
        *result = QByteArray(rawData, rawLength - 2);
        return true;
    }

    // handle Bulk String
    if(result && *respDataType == '$') {
        int length = atoi(rawData);
        rawData = strstr(rawData, "\r") + 2;
        *result = QByteArray(rawData, length);
        return true;
    }

    // handle Array(s)
    if(*respDataType == '*') {
        rawData--;
        QList<QByteArray*>* currentArray = 0;
        int elementCount = 0;
        auto getMoreDataIfNeeded = [&con, &data](char** rawData, int min = 0) {
            // loop until we have enough data
            while((!min && !strstr(*rawData, "\n")) || (min && data.length() - (*rawData - data.data()) < min)) {
                // remove allready parsed data from cache
                data.remove(0, *rawData - data.data());

                // if no data is present so we wait for it
                if(!con->socket->bytesAvailable()) con->socket->waitForReadyRead();

                // read all available data and update the raw pointer
                data += con->socket->readAll();
                *rawData = data.data();
            }
        };

        while(true) {
            // check if we need more data
            getMoreDataIfNeeded(&rawData);

            // parse packet header
            char packetType = *rawData++;
            int length = atoi(rawData);
            rawData = strstr(rawData, "\r") + 2;

            // if we have a collection packet
            if(packetType == '*') {
                elementCount = length;
                if(!currentArray) currentArray = lstResultArray1;
                else currentArray = lstResultArray2;

                // stop parsing if data is not wanted by the caller
                if(!currentArray) break;
                continue;
            }

            // otherwise parse the string packet
            else {
                getMoreDataIfNeeded(&rawData, length + 2);
                currentArray->append(new QByteArray(rawData, length));
                rawData += length + 2;
            }

            // if we have no elements left, exit
            if(!--elementCount) break;
        }
        return true;
    }

    // otherwise we have an parse error
    return false;
}
