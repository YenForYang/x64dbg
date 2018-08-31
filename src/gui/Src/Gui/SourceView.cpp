#include "SourceView.h"
#include <QFileDialog>
#include <QMessageBox>
#include <QDesktopServices>
#include <QProcess>
#include <memory>

struct IBufferedFileReader
{
    enum Direction
    {
        Right,
        Left
    };

    virtual ~IBufferedFileReader() {}
    virtual bool isopen() const = 0;
    virtual bool read(void* dest, uint64_t index, size_t size) = 0;
    virtual uint64_t size() = 0;
    virtual void setbuffersize(size_t size) = 0;
    virtual void setbufferdirection(Direction direction) = 0;

    bool readchar(uint64_t index, char & ch)
    {
        return read(&ch, index, 1);
    }

    bool readstring(uint64_t index, size_t size, std::string & str)
    {
        str.resize(size);
        return read((char*)str.c_str(), index, size);
    }

    bool readvector(uint64_t index, size_t size, std::vector<char> & vec)
    {
        vec.resize(size);
        return read(vec.data(), index, size);
    }
};

class HandleFileReader : public IBufferedFileReader
{
    HANDLE mHandle = INVALID_HANDLE_VALUE;
    uint64_t mFileSize = -1;

    std::vector<char> mBuffer;
    size_t mBufferIndex = 0;
    size_t mBufferSize = 0;
    Direction mBufferDirection = Right;

    bool readnobuffer(void* dest, uint64_t index, size_t size)
    {
        if(!isopen())
            return false;

        LARGE_INTEGER distance;
        distance.QuadPart = index;
        if(!SetFilePointerEx(mHandle, distance, nullptr, FILE_BEGIN))
            return false;

        DWORD read = 0;
        return !!ReadFile(mHandle, dest, (DWORD)size, &read, nullptr);
    }

public:
    HandleFileReader(const wchar_t* szFileName)
    {
        mHandle = CreateFileW(szFileName, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if(mHandle != INVALID_HANDLE_VALUE)
        {
            LARGE_INTEGER size;
            if(GetFileSizeEx(mHandle, &size))
            {
                mFileSize = size.QuadPart;
            }
            else
            {
                CloseHandle(mHandle);
                mHandle = INVALID_HANDLE_VALUE;
            }
        }
    }

    ~HandleFileReader() override
    {
        if(isopen())
        {
            CloseHandle(mHandle);
            mHandle = INVALID_HANDLE_VALUE;
        }
    }

    HandleFileReader(const HandleFileReader &) = delete;

    bool isopen() const override
    {
        return mHandle != INVALID_HANDLE_VALUE;
    }

    bool read(void* dest, uint64_t index, size_t size) override
    {
        if(index + size > mFileSize)
            return false;

        if(size > mBufferSize)
            return readnobuffer(dest, index, size);

        if(index < mBufferIndex || index + size > mBufferIndex + mBuffer.size())
        {
            auto bufferSize = std::min(uint64_t(mBufferSize), mFileSize - index);
            mBuffer.resize(size_t(bufferSize));
            mBufferIndex = size_t(index);
            /*if (mBufferDirection == Left)
            {
                if (mBufferIndex >= mBufferSize + size)
                    mBufferIndex -= mBufferSize + size;
            }*/
            if(!readnobuffer(mBuffer.data(), mBufferIndex, mBuffer.size()))
                return false;
        }

        if(size == 1)
        {
            *(unsigned char*)dest = mBuffer[index - mBufferIndex];
        }
        else
        {
#ifdef _DEBUG
            auto dst = (unsigned char*)dest;
            for(size_t i = 0; i < size; i++)
                dst[i] = mBuffer.at(index - mBufferIndex + i);
#else
            memcpy(dest, mBuffer.data() + (index - mBufferIndex), size);
#endif //_DEBUG
        }

        return true;
    }

    uint64_t size() override
    {
        return mFileSize;
    }

    void setbuffersize(size_t size) override
    {
        mBufferSize = size_t(std::min(uint64_t(size), mFileSize));
    }

    void setbufferdirection(Direction direction) override
    {
        mBufferDirection = direction;
    }
};

class FileLines
{
    std::vector<uint64_t> mLines;
    std::unique_ptr<IBufferedFileReader> mReader;

public:
    bool isopen()
    {
        return mReader && mReader->isopen();
    }

    bool open(const wchar_t* szFileName)
    {
        if(isopen())
            return false;
        mReader = std::make_unique<HandleFileReader>(szFileName);
        return mReader->isopen();
    }

    bool parse()
    {
        if(!isopen())
            return false;
        auto filesize = mReader->size();
        mReader->setbufferdirection(IBufferedFileReader::Right);
        mReader->setbuffersize(10 * 1024 * 1024);
        size_t curIndex = 0, curSize = 0;
        for(uint64_t i = 0; i < filesize; i++)
        {
            /*if (mLines.size() % 100000 == 0)
                printf("%zu\n", i);*/
            char ch;
            if(!mReader->readchar(i, ch))
                return false;
            if(ch == '\r')
                continue;
            if(ch == '\n')
            {
                mLines.push_back(curIndex);
                curIndex = i + 1;
                curSize = 0;
                continue;
            }
            curSize++;
        }
        if(curSize > 0)
            mLines.push_back(curIndex);
        mLines.push_back(filesize + 1);
        return true;
    }

    size_t size() const
    {
        return mLines.size() - 1;
    }

    std::string operator[](size_t index)
    {
        auto lineStart = mLines[index];
        auto nextLineStart = mLines[index + 1];
        std::string result;
        mReader->readstring(lineStart, nextLineStart - lineStart - 1, result);
        while(!result.empty() && result.back() == '\r')
            result.pop_back();
        return result;
    }
};

SourceView::SourceView(QString path, duint addr, QWidget* parent)
    : AbstractStdTable(parent),
      mSourcePath(path),
      mModBase(DbgFunctions()->ModBaseFromAddr(addr))
{
    enableMultiSelection(true);
    enableColumnSorting(false);
    setDrawDebugOnly(false);
    setAddressColumn(0);

    int charwidth = getCharWidth();

    addColumnAt(8 + charwidth * sizeof(duint) * 2, tr("Address"), false);
    addColumnAt(8 + charwidth * 8, tr("Line"), false);
    addColumnAt(0, tr("Code"), false);
    loadColumnFromConfig("SourceView");
    setupContextMenu();

    connect(this, SIGNAL(contextMenuSignal(QPoint)), this, SLOT(contextMenuSlot(QPoint)));

    Initialize();

    loadFile();
}

SourceView::~SourceView()
{
    delete mFileLines;
}

QString SourceView::getCellContent(int r, int c)
{
    if(!isValidIndex(r, c))
        return QString();
    LineData & line = mLines.at(r - mPrepareTableOffset);
    switch(c)
    {
    case ColAddr:
        return line.addr ? ToPtrString(line.addr) : QString();
    case ColLine:
        return QString("%1").arg(line.index + 1);
    case ColCode:
        return line.code.code;
    }
    __debugbreak();
    return "INVALID";
}

bool SourceView::isValidIndex(int r, int c)
{
    if(!mFileLines)
        return false;
    if(c < ColAddr || c > ColCode)
        return false;
    return r >= 0 && size_t(r) < mFileLines->size();
}

void SourceView::sortRows(int column, bool ascending)
{
    Q_UNUSED(column);
    Q_UNUSED(ascending);
}

void SourceView::prepareData()
{
    AbstractTableView::prepareData();
    if(mFileLines)
    {
        auto lines = getNbrOfLineToPrint();
        mPrepareTableOffset = getTableOffset();
        mLines.clear();
        mLines.resize(lines);
        for(auto i = 0; i < lines; i++)
            parseLine(mPrepareTableOffset + i, mLines[i]);
    }
}

void SourceView::setSelection(duint addr)
{
    int line = 0;
    if(!DbgFunctions()->GetSourceFromAddr(addr, nullptr, &line))
        return;
    scrollSelect(line - 1);
    reloadData(); //repaint
}

QString SourceView::getSourcePath()
{
    return mSourcePath;
}

void SourceView::contextMenuSlot(const QPoint & pos)
{
    QMenu wMenu(this);
    mMenuBuilder->build(&wMenu);
    wMenu.exec(mapToGlobal(pos));
}

void SourceView::openSourceFileSlot()
{
    QDesktopServices::openUrl(QUrl::fromLocalFile(mSourcePath));
}

void SourceView::showInDirectorySlot()
{
    QStringList args;
    args << "/select," << QDir::toNativeSeparators(mSourcePath);
    auto process = new QProcess(this);
    process->start("explorer.exe", args);
}

void SourceView::setupContextMenu()
{
    mMenuBuilder = new MenuBuilder(this);
    mMenuBuilder->addAction(makeAction(DIcon("source.png"), tr("Open source file"), SLOT(openSourceFileSlot())));
    mMenuBuilder->addAction(makeAction(DIcon("source_show_in_folder.png"), tr("Show source file in directory"), SLOT(showInDirectorySlot())));
    mMenuBuilder->loadFromConfig();
}

void SourceView::parseLine(size_t index, LineData & line)
{
    QString lineText = QString::fromStdString((*mFileLines)[index]);
    line.addr = DbgFunctions()->GetAddrFromLineEx(mModBase, mSourcePath.toUtf8().constData(), int(index + 1));
    line.index = index;
    line.code.code = lineText.replace('\t', "    "); //TODO: add syntax highlighting
}

void SourceView::loadFile()
{
    if(!mSourcePath.length())
        return;
    if(mFileLines)
    {
        delete mFileLines;
        mFileLines = nullptr;
    }
    mFileLines = new FileLines();
    mFileLines->open(mSourcePath.toStdWString().c_str());
    if(!mFileLines->isopen())
    {
        QMessageBox::warning(this, "Error", "Failed to open file!");
        delete mFileLines;
        mFileLines = nullptr;
        return;
    }
    if(!mFileLines->parse())
    {
        QMessageBox::warning(this, "Error", "Failed to parse file!");
        delete mFileLines;
        mFileLines = nullptr;
        return;
    }
    setRowCount(mFileLines->size());
    setTableOffset(0);
    reloadData();
}
